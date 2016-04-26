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

gboolean bd_btrfs_create_volume (const gchar **devices, const gchar *label, const gchar *data_level, const gchar *md_level, const BDExtraArg **extra, GError **error);
gboolean bd_btrfs_add_device (const gchar *mountpoint, const gchar *device, const BDExtraArg **extra, GError **error);
gboolean bd_btrfs_remove_device (const gchar *mountpoint, const gchar *device, const BDExtraArg **extra, GError **error);
gboolean bd_btrfs_create_subvolume (const gchar *mountpoint, const gchar *name, const BDExtraArg **extra, GError **error);
gboolean bd_btrfs_delete_subvolume (const gchar *mountpoint, const gchar *name, const BDExtraArg **extra, GError **error);
guint64 bd_btrfs_get_default_subvolume_id (const gchar *mountpoint, GError **error);
gboolean bd_btrfs_set_default_subvolume (const gchar *mountpoint, guint64 subvol_id, const BDExtraArg **extra, GError **error);
gboolean bd_btrfs_create_snapshot (const gchar *source, const gchar *dest, gboolean ro, const BDExtraArg **extra, GError **error);
BDBtrfsDeviceInfo** bd_btrfs_list_devices (const gchar *device, GError **error);
BDBtrfsSubvolumeInfo** bd_btrfs_list_subvolumes (const gchar *mountpoint, gboolean snapshots_only, GError **error);
BDBtrfsFilesystemInfo* bd_btrfs_filesystem_info (const gchar *device, GError **error);

gboolean bd_btrfs_mkfs (const gchar **devices, const gchar *label, const gchar *data_level, const gchar *md_level, const BDExtraArg **extra, GError **error);
gboolean bd_btrfs_resize (const gchar *mountpoint, guint64 size, const BDExtraArg **extra, GError **error);
gboolean bd_btrfs_check (const gchar *device, const BDExtraArg **extra, GError **error);
gboolean bd_btrfs_repair (const gchar *device, const BDExtraArg **extra, GError **error);
gboolean bd_btrfs_change_label (const gchar *mountpoint, const gchar *label, GError **error);

#endif  /* BD_BTRFS */
