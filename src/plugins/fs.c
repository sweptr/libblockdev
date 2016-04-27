/*
 * Copyright (C) 2016  Red Hat, Inc.
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

#include <exec.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <blkid.h>
#include <ctype.h>

#include "fs.h"

/**
 * SECTION: fs
 * @short_description: plugin for operations with file systems
 * @title: FS
 * @include: fs.h
 *
 * A plugin for operations with file systems
 */

/**
 * bd_fs_error_quark: (skip)
 */
GQuark bd_fs_error_quark (void)
{
    return g_quark_from_static_string ("g-bd-fs-error-quark");
}

/**
 * bd_fs_ext4_info_copy: (skip)
 *
 * Creates a new copy of @data.
 */
BDFSExt4Info* bd_fs_ext4_info_copy (BDFSExt4Info *data) {
    BDFSExt4Info *ret = g_new0 (BDFSExt4Info, 1);

    ret->label = g_strdup (data->label);
    ret->uuid = g_strdup (data->uuid);
    ret->state = g_strdup (data->state);
    ret->block_size = data->block_size;
    ret->block_count = data->block_count;
    ret->free_blocks = data->free_blocks;

    return ret;
}

/**
 * bd_fs_ext4_info_free: (skip)
 *
 * Frees @data.
 */
void bd_fs_ext4_info_free (BDFSExt4Info *data) {
    g_free (data->label);
    g_free (data->uuid);
    g_free (data->state);
    g_free (data);
}

/**
 * bd_fs_xfs_info_copy: (skip)
 *
 * Creates a new copy of @data.
 */
BDFSXfsInfo* bd_fs_xfs_info_copy (BDFSXfsInfo *data) {
    BDFSXfsInfo *ret = g_new0 (BDFSXfsInfo, 1);

    ret->label = g_strdup (data->label);
    ret->uuid = g_strdup (data->uuid);
    ret->block_size = data->block_size;
    ret->block_count = data->block_count;

    return ret;
}

/**
 * bd_fs_xfs_info_free: (skip)
 *
 * Frees @data.
 */
void bd_fs_xfs_info_free (BDFSXfsInfo *data) {
    g_free (data->label);
    g_free (data->uuid);
    g_free (data);
}

/**
 * check: (skip)
 */
gboolean check() {
    GError *error = NULL;
    gboolean ret = bd_utils_check_util_version ("mkfs.ext4", NULL, "", NULL, &error);

    if (!ret && error) {
        g_warning("Cannot load the FS plugin: %s" , error->message);
        g_clear_error (&error);
    }
    return ret;
}

static gint synced_close (gint fd) {
    gint ret = 0;
    ret = fsync (fd);
    if (close (fd) != 0)
        ret = 1;
    return ret;
}

/**
 * bd_fs_wipe:
 * @device: the device to wipe signatures from
 * @all: whether to wipe all (%TRUE) signatures or just the first (%FALSE) one
 * @error: (out): place to store error (if any)
 *
 * Returns: whether signagures were successfully wiped on @device or not
 */
gboolean bd_fs_wipe (const gchar *device, gboolean all, GError **error) {
    blkid_probe probe = NULL;
    gint fd = 0;
    gint status = 0;

    probe = blkid_new_probe ();
    if (!probe) {
        g_set_error (error, BD_FS_ERROR, BD_FS_ERROR_FAIL,
                     "Failed to create a probe for the device '%s'", device);
        return FALSE;
    }

    fd = open (device, O_RDWR|O_CLOEXEC);
    if (fd == -1) {
        g_set_error (error, BD_FS_ERROR, BD_FS_ERROR_FAIL,
                     "Failed to create a probe for the device '%s'", device);
        blkid_free_probe (probe);
        return FALSE;
    }

    status = blkid_probe_set_device (probe, fd, 0, 0);
    if (status != 0) {
        g_set_error (error, BD_FS_ERROR, BD_FS_ERROR_FAIL,
                     "Failed to create a probe for the device '%s'", device);
        blkid_free_probe (probe);
        synced_close (fd);
        return FALSE;
    }

	blkid_probe_enable_partitions(probe, 1);
	blkid_probe_set_partitions_flags(probe, BLKID_PARTS_MAGIC);
	blkid_probe_enable_superblocks(probe, 1);
	blkid_probe_set_superblocks_flags(probe, BLKID_SUBLKS_MAGIC | BLKID_SUBLKS_BADCSUM);

    status = blkid_do_probe (probe);
    if (status != 0) {
        g_set_error (error, BD_FS_ERROR, BD_FS_ERROR_FAIL,
                     "Failed to probe the device '%s'", device);
        blkid_free_probe (probe);
        synced_close (fd);
        return FALSE;
    }

    status = blkid_do_wipe (probe, FALSE);
    if (status != 0) {
        g_set_error (error, BD_FS_ERROR, BD_FS_ERROR_FAIL,
                     "Failed to wipe signatures on the device '%s'", device);
        blkid_free_probe (probe);
        synced_close (fd);
        return FALSE;
    }
    while (all && (blkid_do_probe (probe) == 0)) {
        status = blkid_do_wipe (probe, FALSE);
        if (status != 0) {
            g_set_error (error, BD_FS_ERROR, BD_FS_ERROR_FAIL,
                         "Failed to wipe signatures on the device '%s'", device);
            blkid_free_probe (probe);
            synced_close (fd);
            return FALSE;
        }
    }

    blkid_free_probe (probe);
    synced_close (fd);

    return TRUE;
}

static gboolean wipe_fs (const gchar *device, const gchar *fs_type, GError **error) {
    blkid_probe probe = NULL;
    gint fd = 0;
    gint status = 0;
    const gchar *value = NULL;
    size_t len = 0;

    probe = blkid_new_probe ();
    if (!probe) {
        g_set_error (error, BD_FS_ERROR, BD_FS_ERROR_FAIL,
                     "Failed to create a probe for the device '%s'", device);
        return FALSE;
    }

    fd = open (device, O_RDWR|O_CLOEXEC);
    if (fd == -1) {
        g_set_error (error, BD_FS_ERROR, BD_FS_ERROR_FAIL,
                     "Failed to create a probe for the device '%s'", device);
        blkid_free_probe (probe);
        return FALSE;
    }

    status = blkid_probe_set_device (probe, fd, 0, 0);
    if (status != 0) {
        g_set_error (error, BD_FS_ERROR, BD_FS_ERROR_FAIL,
                     "Failed to create a probe for the device '%s'", device);
        blkid_free_probe (probe);
        synced_close (fd);
        return FALSE;
    }

	blkid_probe_enable_partitions(probe, 1);
	blkid_probe_set_partitions_flags(probe, BLKID_PARTS_MAGIC);
	blkid_probe_enable_superblocks(probe, 1);
	blkid_probe_set_superblocks_flags(probe, BLKID_SUBLKS_USAGE | BLKID_SUBLKS_TYPE |
                                             BLKID_SUBLKS_MAGIC | BLKID_SUBLKS_BADCSUM);

    status = blkid_do_probe (probe);
    if (status != 0) {
        g_set_error (error, BD_FS_ERROR, BD_FS_ERROR_FAIL,
                     "Failed to probe the device '%s'", device);
        blkid_free_probe (probe);
        synced_close (fd);
        return FALSE;
    }

    status = blkid_probe_lookup_value (probe, "USAGE", &value, NULL);
    if (status != 0) {
        g_set_error (error, BD_FS_ERROR, BD_FS_ERROR_FAIL,
                     "Failed to get signature type for the device '%s'", device);
        blkid_free_probe (probe);
        synced_close (fd);
        return FALSE;
    }

    if (strncmp (value, "filesystem", 10) != 0) {
        g_set_error (error, BD_FS_ERROR, BD_FS_ERROR_INVAL,
                     "The signature on the device '%s' is of type '%s', not 'filesystem'", device, value);
        blkid_free_probe (probe);
        synced_close (fd);
        return FALSE;
    }

    if (fs_type) {
        status = blkid_probe_lookup_value (probe, "TYPE", &value, &len);
        if (status != 0) {
            g_set_error (error, BD_FS_ERROR, BD_FS_ERROR_FAIL,
                         "Failed to get filesystem type for the device '%s'", device);
            blkid_free_probe (probe);
            synced_close (fd);
            return FALSE;
        }

        if (strncmp (value, fs_type, len-1) != 0) {
            g_set_error (error, BD_FS_ERROR, BD_FS_ERROR_INVAL,
                         "The file system type on the device '%s' is '%s', not '%s'", device, value, fs_type);
            blkid_free_probe (probe);
            synced_close (fd);
            return FALSE;
        }
    }

    status = blkid_do_wipe (probe, FALSE);
    if (status != 0) {
        g_set_error (error, BD_FS_ERROR, BD_FS_ERROR_FAIL,
                     "Failed to wipe the filesystem signature on the device '%s'", device);
        blkid_free_probe (probe);
        synced_close (fd);
        return FALSE;
    }

    blkid_free_probe (probe);
    synced_close (fd);
    return TRUE;
}

/**
 * bd_fs_ext4_mkfs:
 * @device: the device to create a new ext4 fs on
 * @extra: (allow-none) (array zero-terminated=1): extra options for the creation (right now
 *                                                 passed to the 'mkfs.ext4' utility)
 * @error: (out): place to store error (if any)
 *
 * Returns: whether a new ext4 fs was successfully created on @device or not
 */
gboolean bd_fs_ext4_mkfs (const gchar *device, const BDExtraArg **extra, GError **error) {
    const gchar *args[3] = {"mkfs.ext4", device, NULL};

    return bd_utils_exec_and_report_error (args, extra, error);
}

/**
 * bd_fs_ext4_wipe:
 * @device: the device to wipe an ext4 signature from
 * @error: (out): place to store error (if any)
 *
 * Returns: whether an ext4 signature was successfully wiped from the @device or
 *          not
 */
gboolean bd_fs_ext4_wipe (const gchar *device, GError **error) {
    return wipe_fs (device, "ext4", error);
}

/**
 * bd_fs_ext4_check:
 * @device: the device the file system on which to check
 * @extra: (allow-none) (array zero-terminated=1): extra options for the check (right now
 *                                                 passed to the 'e2fsck' utility)
 * @error: (out): place to store error (if any)
 *
 * Returns: whether an ext4 file system on the @device is clean or not
 */
gboolean bd_fs_ext4_check (const gchar *device, const BDExtraArg **extra, GError **error) {
    /* Force checking even if the file system seems clean. AND
     * Open the filesystem read-only, and assume an answer of no to all
     * questions. */
    const gchar *args[5] = {"e2fsck", "-f", "-n", device, NULL};
    gint status = 0;
    gboolean ret = FALSE;

    ret = bd_utils_exec_and_report_status_error (args, extra, &status, error);
    if (!ret && (status == 4)) {
        /* no error should be reported for exit code 4 - File system errors left uncorrected */
        g_clear_error (error);
    }
    return ret;
}

/**
 * bd_fs_ext4_repair:
 * @device: the device the file system on which to repair
 * @unsafe: whether to do unsafe operations too
 * @extra: (allow-none) (array zero-terminated=1): extra options for the repair (right now
 *                                                 passed to the 'e2fsck' utility)
 * @error: (out): place to store error (if any)
 *
 * Returns: whether an ext4 file system on the @device was successfully repaired
 *          (if needed) or not (error is set in that case)
 */
gboolean bd_fs_ext4_repair (const gchar *device, gboolean unsafe, const BDExtraArg **extra, GError **error) {
    /* Force checking even if the file system seems clean. AND
     *     Automatically repair what can be safely repaired. OR
     *     Assume an answer of `yes' to all questions. */
    const gchar *args[5] = {"e2fsck", "-f", unsafe ? "-y" : "-p", device, NULL};

    return bd_utils_exec_and_report_error (args, extra, error);
}

/**
 * bd_fs_ext4_set_label:
 * @device: the device the file system on which to set label for
 * @label: label to set
 * @error: (out): place to store error (if any)
 *
 * Returns: whether the label of ext4 file system on the @device was
 *          successfully set or not
 */
gboolean bd_fs_ext4_set_label (const gchar *device, const gchar *label, GError **error) {
    const gchar *args[5] = {"tune2fs", "-L", label, device, NULL};

    return bd_utils_exec_and_report_error (args, NULL, error);
}

/**
 * parse_output_vars: (skip)
 * @str: string to parse
 * @item_sep: item separator(s) (key-value pairs separator)
 * @key_val_sep: key-value separator(s) (typically ":" or "=")
 * @num_items: (out): number of parsed items (key-value pairs)
 *
 * Returns: (transfer full): GHashTable containing the key-value pairs parsed
 * from the @str.
 */
static GHashTable* parse_output_vars (const gchar *str, const gchar *item_sep, const gchar *key_val_sep, guint *num_items) {
    GHashTable *table = NULL;
    gchar **items = NULL;
    gchar **item_p = NULL;
    gchar **key_val = NULL;

    table = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
    *num_items = 0;

    items = g_strsplit_set (str, item_sep, 0);
    for (item_p=items; *item_p; item_p++) {
        key_val = g_strsplit (*item_p, key_val_sep, 2);
        if (g_strv_length (key_val) == 2) {
            /* we only want to process valid lines (with the separator) */
            g_hash_table_insert (table, g_strstrip (key_val[0]), g_strstrip (key_val[1]));
            (*num_items)++;
        } else
            /* invalid line, just free key_val */
            g_strfreev (key_val);
    }

    g_strfreev (items);
    return table;
}

static BDFSExt4Info* get_ext4_info_from_table (GHashTable *table, gboolean free_table) {
    BDFSExt4Info *ret = g_new0 (BDFSExt4Info, 1);
    gchar *value = NULL;

    ret->label = g_strdup ((gchar*) g_hash_table_lookup (table, "Filesystem volume name"));
    if ((!ret->label) || (g_strcmp0 (ret->label, "<none>") == 0))
        ret->label = g_strdup ("");
    ret->uuid = g_strdup ((gchar*) g_hash_table_lookup (table, "Filesystem UUID"));
    ret->state = g_strdup ((gchar*) g_hash_table_lookup (table, "Filesystem state"));

    value = (gchar*) g_hash_table_lookup (table, "Block size");
    if (value)
        ret->block_size = g_ascii_strtoull (value, NULL, 0);
    else
        ret->block_size = 0;
    value = (gchar*) g_hash_table_lookup (table, "Block count");
    if (value)
        ret->block_count = g_ascii_strtoull (value, NULL, 0);
    else
        ret->block_count = 0;
    value = (gchar*) g_hash_table_lookup (table, "Free blocks");
    if (value)
        ret->free_blocks = g_ascii_strtoull (value, NULL, 0);
    else
        ret->free_blocks = 0;

    if (free_table)
        g_hash_table_destroy (table);

    return ret;
}

/**
 * bd_fs_ext4_get_info:
 * @device: the device the file system of which to get info for
 * @error: (out): place to store error (if any)
 *
 * Returns: (transfer full): information about the file system on @device or
 *                           %NULL in case of error
 */
BDFSExt4Info* bd_fs_ext4_get_info (const gchar *device, GError **error) {
    const gchar *args[4] = {"dumpe2fs", "-h", device, NULL};
    gboolean success = FALSE;
    gchar *output = NULL;
    GHashTable *table = NULL;
    guint num_items = 0;
    BDFSExt4Info *ret = NULL;

    success = bd_utils_exec_and_capture_output (args, NULL, &output, error);
    if (!success) {
        /* error is already populated */
        return FALSE;
    }

    table = parse_output_vars (output, "\n", ":", &num_items);
    g_free (output);
    if (!table || (num_items == 0)) {
        /* something bad happened or some expected items were missing  */
        g_set_error (error, BD_FS_ERROR, BD_FS_ERROR_PARSE, "Failed to parse ext4 file system information");
        if (table)
            g_hash_table_destroy (table);
        return NULL;
    }

    ret = get_ext4_info_from_table (table, TRUE);
    if (!ret) {
        g_set_error (error, BD_FS_ERROR, BD_FS_ERROR_PARSE, "Failed to parse ext4 file system information");
        return NULL;
    }

    return ret;
}

/**
 * bd_fs_ext4_resize:
 * @device: the device the file system of which to resize
 * @new_size: new requested size for the file system (if 0, the file system is
 *            adapted to the underlying block device)
 * @extra: (allow-none) (array zero-terminated=1): extra options for the resize (right now
 *                                                 passed to the 'resize2fs' utility)
 * @error: (out): place to store error (if any)
 *
 * Returns: whether the file system on @device was successfully resized or not
 */
gboolean bd_fs_ext4_resize (const gchar *device, guint64 new_size, const BDExtraArg **extra, GError **error) {
    const gchar *args[4] = {"resize2fs", device, NULL, NULL};
    gboolean ret = FALSE;

    if (new_size != 0)
        /* resize2fs doesn't understand bytes, just 512B sectors */
        args[2] = g_strdup_printf ("%"G_GUINT64_FORMAT"s", new_size / 512);
    ret = bd_utils_exec_and_report_error (args, extra, error);

    g_free ((gchar *) args[2]);
    return ret;
}

/**
 * bd_fs_xfs_mkfs:
 * @device: the device to create a new xfs fs on
 * @extra: (allow-none) (array zero-terminated=1): extra options for the creation (right now
 *                                                 passed to the 'mkfs.xfs' utility)
 * @error: (out): place to store error (if any)
 *
 * Returns: whether a new xfs fs was successfully created on @device or not
 */
gboolean bd_fs_xfs_mkfs (const gchar *device, const BDExtraArg **extra, GError **error) {
    const gchar *args[3] = {"mkfs.xfs", device, NULL};

    return bd_utils_exec_and_report_error (args, extra, error);
}

/**
 * bd_fs_xfs_wipe:
 * @device: the device to wipe an xfs signature from
 * @error: (out): place to store error (if any)
 *
 * Returns: whether an xfs signature was successfully wiped from the @device or
 *          not
 */
gboolean bd_fs_xfs_wipe (const gchar *device, GError **error) {
    return wipe_fs (device, "xfs", error);
}

/**
 * bd_fs_xfs_check:
 * @device: the device containing the file system to check
 * @error: (out): place to store error (if any)
 *
 * Returns: whether an xfs file system on the @device is clean or not
 *
 * Note: if the file system is mounted it may be reported as unclean even if
 *       everything is okay and there are just some pending/in-progress writes
 */
gboolean bd_fs_xfs_check (const gchar *device, GError **error) {
    const gchar *args[6] = {"xfs_db", "-r", "-c", "check", device, NULL};
    gboolean ret = FALSE;

    ret = bd_utils_exec_and_report_error (args, NULL, error);
    if (!ret && *error &&  g_error_matches ((*error), BD_UTILS_EXEC_ERROR, BD_UTILS_EXEC_ERROR_FAILED))
        /* non-zero exit status -> the fs is not clean, but not an error */
        /* TODO: should we check that the device exists and contains an XFS FS beforehand? */
        g_clear_error (error);
    return ret;
}

/**
 * bd_fs_xfs_repair:
 * @device: the device containing the file system to repair
 * @extra: (allow-none) (array zero-terminated=1): extra options for the repair (right now
 *                                                 passed to the 'xfs_repair' utility)
 * @error: (out): place to store error (if any)
 *
 * Returns: whether an xfs file system on the @device was successfully repaired
 *          (if needed) or not (error is set in that case)
 */
gboolean bd_fs_xfs_repair (const gchar *device, const BDExtraArg **extra, GError **error) {
    const gchar *args[3] = {"xfs_repair", device, NULL};

    return bd_utils_exec_and_report_error (args, extra, error);
}

/**
 * bd_fs_xfs_set_label:
 * @device: the device containing the file system to set label for
 * @label: label to set
 * @error: (out): place to store error (if any)
 *
 * Returns: whether the label of xfs file system on the @device was
 *          successfully set or not
 */
gboolean bd_fs_xfs_set_label (const gchar *device, const gchar *label, GError **error) {
    const gchar *args[5] = {"xfs_admin", "-L", label, device, NULL};
    if (!label || (strncmp (label, "", 1) == 0))
        args[2] = "--";

    return bd_utils_exec_and_report_error (args, NULL, error);
}

/**
 * bd_fs_xfs_get_info:
 * @device: the device containing the file system to get info for
 * @error: (out): place to store error (if any)
 *
 * Returns: (transfer full): information about the file system on @device or
 *                           %NULL in case of error
 */
BDFSXfsInfo* bd_fs_xfs_get_info (const gchar *device, GError **error) {
    const gchar *args[4] = {"xfs_admin", "-lu", device, NULL};
    gboolean success = FALSE;
    gchar *output = NULL;
    BDFSXfsInfo *ret = NULL;
    gchar **lines = NULL;
    gchar **line_p = NULL;
    gboolean have_label = FALSE;
    gboolean have_uuid = FALSE;
    gchar *val_start = NULL;
    gchar *val_end = NULL;

    success = bd_utils_exec_and_capture_output (args, NULL, &output, error);
    if (!success)
        /* error is already populated */
        return FALSE;

    ret = g_new0 (BDFSXfsInfo, 1);
    lines = g_strsplit (output, "\n", 0);
    g_free (output);
    for (line_p=lines; *line_p && (!have_label || !have_uuid); line_p++) {
        if (!have_label && g_str_has_prefix (*line_p, "label")) {
            /* extract label from something like this: label = "TEST_LABEL" */
            val_start = strchr (*line_p, '"');
            if (val_start)
                val_end = strchr(val_start + 1, '"');
            if (val_start && val_end) {
                ret->label = g_strndup (val_start + 1, val_end - val_start - 1);
                have_label = TRUE;
            }
        } else if (!have_uuid && g_str_has_prefix (*line_p, "UUID")) {
            /* get right after the "UUID = " prefix */
            val_start = *line_p + 7;
            ret->uuid = g_strdup (val_start);
            have_uuid = TRUE;
        }
    }
    g_strfreev (lines);

    args[0] = "xfs_info";
    args[1] = device;
    args[2] = NULL;
    success = bd_utils_exec_and_capture_output (args, NULL, &output, error);
    if (!success) {
        /* error is already populated */
        bd_fs_xfs_info_free (ret);
        return FALSE;
    }

    lines = g_strsplit (output, "\n", 0);
    g_free (output);
    line_p = lines;
    /* find the beginning of the (data) section we are interested in */
    while (*line_p && !g_str_has_prefix (*line_p, "data"))
        line_p++;
    if (!line_p) {
        /* error is already populated */
        g_set_error (error, BD_FS_ERROR, BD_FS_ERROR_PARSE, "Failed to parse xfs file system information");
        g_strfreev (lines);
        bd_fs_xfs_info_free (ret);
        return FALSE;
    }

    /* extract data from something like this: "data     =      bsize=4096   blocks=262400, imaxpct=25" */
    val_start = strchr (*line_p, '=');
    val_start++;
    while (isspace (*val_start))
        val_start++;
    if (g_str_has_prefix (val_start, "bsize")) {
        val_start = strchr (val_start, '=');
        val_start++;
        ret->block_size = g_ascii_strtoull (val_start, NULL, 0);
    } else {
        /* error is already populated */
        g_set_error (error, BD_FS_ERROR, BD_FS_ERROR_PARSE, "Failed to parse xfs file system information");
        g_strfreev (lines);
        bd_fs_xfs_info_free (ret);
        return FALSE;
    }
    while (isdigit (*val_start) || isspace(*val_start))
        val_start++;
    if (g_str_has_prefix (val_start, "blocks")) {
        val_start = strchr (val_start, '=');
        val_start++;
        ret->block_count = g_ascii_strtoull (val_start, NULL, 0);
    } else {
        /* error is already populated */
        g_set_error (error, BD_FS_ERROR, BD_FS_ERROR_PARSE, "Failed to parse xfs file system information");
        g_strfreev (lines);
        bd_fs_xfs_info_free (ret);
        return FALSE;
    }
    g_strfreev (lines);

    return ret;
}

/**
 * bd_fs_xfs_resize:
 * @mpoint: the mount point of the file system to resize
 * @new_size: new requested size for the file system *in file system blocks* (see bd_fs_xfs_get_info())
 *            (if 0, the file system is adapted to the underlying block device)
 * @extra: (allow-none) (array zero-terminated=1): extra options for the resize (right now
 *                                                 passed to the 'xfs_growfs' utility)
 * @error: (out): place to store error (if any)
 *
 * Returns: whether the file system mounted on @mpoint was successfully resized or not
 */
gboolean bd_fs_xfs_resize (const gchar *mpoint, guint64 new_size, const BDExtraArg **extra, GError **error) {
    const gchar *args[5] = {"xfs_growfs", NULL, NULL, NULL, NULL};
    gchar *size_str = NULL;
    gboolean ret = FALSE;

    if (new_size != 0) {
        args[1] = "-D";
        /* xfs_growfs doesn't understand bytes, just a number of blocks */
        size_str = g_strdup_printf ("%"G_GUINT64_FORMAT, new_size);
        args[2] = size_str;
        args[3] = mpoint;
    } else
        args[1] = mpoint;

    ret = bd_utils_exec_and_report_error (args, extra, error);

    g_free (size_str);
    return ret;
}
