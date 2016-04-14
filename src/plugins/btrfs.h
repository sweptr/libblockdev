#include <glib.h>
#include <glib-object.h>
#include <utils.h>

#ifndef BD_BTRFS
#define BD_BTRFS

#define BTRFS_MIN_VERSION "3.18.2"

#define BD_BTRFS_MAIN_VOLUME_ID 5
#define BD_BTRFS_MIN_MEMBER_SIZE (128 MiB)

GQuark bd_btrfs_error_quark (void);
#define BD_BTRFS_ERROR bd_btrfs_error_quark ()
typedef enum {
    BD_BTRFS_ERROR_DEVICE,
    BD_BTRFS_ERROR_PARSE,
} BDBtrfsError;

typedef struct BDBtrfsDeviceInfo {
    guint64 id;
    gchar *path;
    guint64 size;
    guint64 used;
} BDBtrfsDeviceInfo;

void bd_btrfs_device_info_free (BDBtrfsDeviceInfo *info);
BDBtrfsDeviceInfo* bd_btrfs_device_info_copy (BDBtrfsDeviceInfo *info);

typedef struct BDBtrfsSubvolumeInfo {
    guint64 id;
    guint64 parent_id;
    gchar *path;
} BDBtrfsSubvolumeInfo;

void bd_btrfs_subvolume_info_free (BDBtrfsSubvolumeInfo *info);
BDBtrfsSubvolumeInfo* bd_btrfs_subvolume_info_copy (BDBtrfsSubvolumeInfo *info);

typedef struct BDBtrfsFilesystemInfo {
    gchar *label;
    gchar *uuid;
    guint64 num_devices;
    guint64 used;
} BDBtrfsFilesystemInfo;

void bd_btrfs_filesystem_info_free (BDBtrfsFilesystemInfo *info);
BDBtrfsFilesystemInfo* bd_btrfs_filesystem_info_copy (BDBtrfsFilesystemInfo *info);

gboolean bd_btrfs_create_volume (gchar **devices, gchar *label, gchar *data_level, gchar *md_level, BDExtraArg **extra, GError **error);
gboolean bd_btrfs_add_device (gchar *mountpoint, gchar *device, BDExtraArg **extra, GError **error);
gboolean bd_btrfs_remove_device (gchar *mountpoint, gchar *device, BDExtraArg **extra, GError **error);
gboolean bd_btrfs_create_subvolume (gchar *mountpoint, gchar *name, BDExtraArg **extra, GError **error);
gboolean bd_btrfs_delete_subvolume (gchar *mountpoint, gchar *name, BDExtraArg **extra, GError **error);
guint64 bd_btrfs_get_default_subvolume_id (gchar *mountpoint, GError **error);
gboolean bd_btrfs_set_default_subvolume (gchar *mountpoint, guint64 subvol_id, BDExtraArg **extra, GError **error);
gboolean bd_btrfs_create_snapshot (gchar *source, gchar *dest, gboolean ro, BDExtraArg **extra, GError **error);
BDBtrfsDeviceInfo** bd_btrfs_list_devices (gchar *device, GError **error);
BDBtrfsSubvolumeInfo** bd_btrfs_list_subvolumes (gchar *mountpoint, gboolean snapshots_only, GError **error);
BDBtrfsFilesystemInfo* bd_btrfs_filesystem_info (gchar *device, GError **error);

gboolean bd_btrfs_mkfs (gchar **devices, gchar *label, gchar *data_level, gchar *md_level, BDExtraArg **extra, GError **error);
gboolean bd_btrfs_resize (gchar *mountpoint, guint64 size, BDExtraArg **extra, GError **error);
gboolean bd_btrfs_check (gchar *device, BDExtraArg **extra, GError **error);
gboolean bd_btrfs_repair (gchar *device, BDExtraArg **extra, GError **error);
gboolean bd_btrfs_change_label (gchar *mountpoint, gchar *label, GError **error);

#endif  /* BD_BTRFS */
