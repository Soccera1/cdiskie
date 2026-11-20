#include "cdiskie.h"

#include <stdlib.h>
#include <string.h>

static GDBusProxy *proxy(GDBusObject *object, const char *name)
{
    return (GDBusProxy *)g_dbus_object_get_interface(object, name);
}

static GVariant *property(GDBusProxy *interface, const char *name)
{
    return interface ? g_dbus_proxy_get_cached_property(interface, name) : NULL;
}

static char *string_property(GDBusProxy *interface, const char *name)
{
    g_autoptr(GVariant) value = property(interface, name);
    if (!value)
        return NULL;
    if (g_variant_is_of_type(value, G_VARIANT_TYPE_BYTESTRING))
        return g_variant_dup_bytestring(value, NULL);
    const char *string = g_variant_get_string(value, NULL);
    return strcmp(string, "/") && *string ? g_strdup(string) : NULL;
}

static char **paths_property(GDBusProxy *interface, const char *name)
{
    g_autoptr(GVariant) value = property(interface, name);
    return value ? g_variant_dup_bytestring_array(value, NULL) : g_new0(char *, 1);
}

static gboolean bool_property(GDBusProxy *interface, const char *name)
{
    g_autoptr(GVariant) value = property(interface, name);
    return value && g_variant_get_boolean(value);
}

static gboolean has_interface(GDBusObject *object, const char *name)
{
    g_autoptr(GDBusProxy) interface = proxy(object, name);
    return interface != NULL;
}

static char *find_cleartext(App *app, const char *parent)
{
    char *path = NULL;
    GList *objects = g_dbus_object_manager_get_objects(app->manager);
    for (GList *link = objects; link && !path; link = link->next) {
        g_autoptr(GDBusProxy) block = proxy(link->data, IFACE("Block"));
        g_autofree char *backing = string_property(block, "CryptoBackingDevice");
        if (!g_strcmp0(backing, parent))
            path = g_strdup(g_dbus_object_get_object_path(link->data));
    }
    g_list_free_full(objects, g_object_unref);
    return path;
}

static Device *device_read(App *app, GDBusObject *object)
{
    g_autoptr(GDBusProxy) block = proxy(object, IFACE("Block"));
    g_autoptr(GDBusProxy) fs = proxy(object, IFACE("Filesystem"));
    g_autoptr(GDBusProxy) encrypted = proxy(object, IFACE("Encrypted"));
    g_autoptr(GDBusProxy) partition = proxy(object, IFACE("Partition"));
    g_autoptr(GDBusProxy) loop = proxy(object, IFACE("Loop"));
    g_autoptr(GDBusProxy) drive = proxy(object, IFACE("Drive"));
    if (!block && !drive)
        return NULL;

    Device *device = g_new0(Device, 1);
    device->object = g_object_ref(object);
    device->path = g_strdup(g_dbus_object_get_object_path(object));
    device->block = block != NULL;
    device->filesystem = fs != NULL;
    device->encrypted = encrypted != NULL;
    device->partition = partition != NULL;
    device->partition_table = has_interface(object, IFACE("PartitionTable"));
    device->loop = loop != NULL;

    device->file = string_property(block, "Device");
    device->drive = string_property(block, "Drive");
    device->backing = string_property(block, "CryptoBackingDevice");
    device->table = string_property(partition, "Table");
    if (encrypted) {
        device->cleartext = string_property(encrypted, "CleartextDevice");
        if (!device->cleartext)
            device->cleartext = find_cleartext(app, device->path);
    }
    device->loop_file = string_property(loop, "BackingFile");
    device->label = string_property(block, "IdLabel");
    device->uuid = string_property(block, "IdUUID");
    device->type = string_property(block, "IdType");
    device->usage = string_property(block, "IdUsage");
    device->id = string_property(block, "Id");
    device->presentation = string_property(block, "PreferredDevice");
    device->mounts = paths_property(fs, "MountPoints");
    device->symlinks = paths_property(block, "Symlinks");
    device->system = bool_property(block, "HintSystem");
    device->ignored = bool_property(block, "HintIgnore");
    device->auto_hint = bool_property(block, "HintAuto");
    device->autoclear = bool_property(loop, "Autoclear");
    g_autoptr(GVariant) size = property(block, "Size");
    g_autoptr(GVariant) uid = property(loop, "SetupByUID");
    device->size = size ? g_variant_get_uint64(size) : 0;
    device->uid = uid ? g_variant_get_uint32(uid) : 0;

    if (device->drive && !drive) {
        g_autoptr(GDBusObject) parent = g_dbus_object_manager_get_object(app->manager, device->drive);
        if (parent)
            drive = proxy(parent, IFACE("Drive"));
    }
    device->model = string_property(drive, "Model");
    device->vendor = string_property(drive, "Vendor");
    device->ejectable = bool_property(drive, "Ejectable");
    device->detachable = bool_property(drive, "CanPowerOff");
    device->media = drive ? bool_property(drive, "MediaAvailable") : TRUE;
    if (!device->presentation)
        device->presentation = g_strdup(device->file ? device->file : device->path);
    return device;
}

void device_free(Device *device)
{
    if (!device)
        return;
    g_object_unref(device->object);
    g_free(device->path);
    g_free(device->file);
    g_free(device->drive);
    g_free(device->backing);
    g_free(device->table);
    g_free(device->cleartext);
    g_free(device->loop_file);
    g_free(device->label);
    g_free(device->uuid);
    g_free(device->type);
    g_free(device->usage);
    g_free(device->id);
    g_free(device->presentation);
    g_free(device->model);
    g_free(device->vendor);
    g_strfreev(device->mounts);
    g_strfreev(device->symlinks);
    g_free(device);
}

Device *device_get(App *app, const char *path)
{
    if (!path)
        return NULL;
    g_autoptr(GDBusObject) object = g_dbus_object_manager_get_object(app->manager, path);
    return object ? device_read(app, object) : NULL;
}

static gint compare_paths(gconstpointer a, gconstpointer b)
{
    const Device *first = *(Device *const *)a;
    const Device *second = *(Device *const *)b;
    return strcmp(first->path, second->path);
}

GPtrArray *devices_get(App *app)
{
    GPtrArray *devices = g_ptr_array_new_with_free_func((GDestroyNotify)device_free);
    GList *objects = g_dbus_object_manager_get_objects(app->manager);
    for (GList *link = objects; link; link = link->next) {
        Device *device = device_read(app, link->data);
        if (device)
            g_ptr_array_add(devices, device);
    }
    g_list_free_full(objects, g_object_unref);
    g_ptr_array_sort(devices, compare_paths);
    return devices;
}

Device *device_find(App *app, const char *target)
{
    if (g_str_has_prefix(target, ROOT_PATH "/"))
        return device_get(app, target);
    g_autofree char *absolute = g_canonicalize_filename(target, NULL);
    char *real = realpath(absolute, NULL);
    g_autofree char *resolved = g_strdup(real ? real : absolute);
    free(real);

    g_autoptr(GPtrArray) devices = devices_get(app);
    for (guint i = 0; i < devices->len; i++) {
        Device *device = devices->pdata[i];
        const char *uuid = g_str_has_prefix(target, "UUID=") ? target + 5 : target;
        g_autoptr(GDBusProxy) partition = proxy(device->object, IFACE("Partition"));
        g_autofree char *partition_uuid = string_property(partition, "UUID");
        if ((device->uuid && !g_ascii_strcasecmp(device->uuid, uuid)) ||
            (partition_uuid && !g_ascii_strcasecmp(partition_uuid, uuid)) ||
            !g_strcmp0(device->file, resolved) || !g_strcmp0(device->loop_file, resolved) ||
            g_strv_contains((const char *const *)device->symlinks, absolute) ||
            g_strv_contains((const char *const *)device->mounts, resolved) ||
            g_strv_contains((const char *const *)device->mounts, absolute))
            return g_ptr_array_steal_index(devices, i);
    }
    return NULL;
}

const char *device_parent(Device *device)
{
    return device->table ? device->table : device->backing;
}

static char **one_value(const char *value)
{
    char **array = g_new0(char *, 2);
    array[0] = g_strdup(value ? value : "");
    return array;
}

char **device_values(Device *device, const char *key)
{
    const char *label = device->label ? device->label : device->presentation;
    g_autofree char *drive_label = g_strjoin(" ", device->vendor ? device->vendor : "",
                                            device->model ? device->model : "", NULL);
    struct { const char *key, *value; } strings[] = {
        { "device_file", device->file }, { "device_presentation", device->presentation },
        { "device_id", device->id }, { "id_type", device->type }, { "id_usage", device->usage },
        { "id_label", device->label }, { "id_uuid", device->uuid },
        { "loop_file", device->loop_file }, { "mount_path", device->mounts[0] },
        { "drive_model", device->model }, { "drive_vendor", device->vendor },
        { "drive_label", g_strstrip(drive_label) }, { "ui_label", label },
        { "ui_device_label", label }, { "ui_device_presentation", device->presentation },
        { "ui_id_label", device->label }, { "ui_id_uuid", device->uuid },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(strings); i++)
        if (!strcmp(key, strings[i].key))
            return one_value(strings[i].value);

    struct { const char *key; gboolean value; } booleans[] = {
        { "is_drive", !device->block }, { "is_block", device->block },
        { "is_partition_table", device->partition_table }, { "is_partition", device->partition },
        { "is_filesystem", device->filesystem }, { "is_luks", device->encrypted },
        { "is_crypto", device->encrypted }, { "is_loop", device->loop },
        { "is_toplevel", !device->backing && !device->partition },
        { "is_detachable", device->detachable }, { "is_ejectable", device->ejectable },
        { "has_media", device->media }, { "is_ignored", device->ignored },
        { "is_luks_cleartext", device->backing != NULL }, { "is_external", !device->system },
        { "is_systeminternal", device->system }, { "is_mounted", device->mounts[0] != NULL },
        { "is_unlocked", device->cleartext != NULL },
        { "in_use", device->mounts[0] || device->cleartext },
        { "should_automount", device->auto_hint }, { "autoclear", device->autoclear },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(booleans); i++)
        if (!strcmp(key, booleans[i].key))
            return one_value(booleans[i].value ? "true" : "false");
    if (!strcmp(key, "mount_paths"))
        return g_strdupv(device->mounts);
    if (!strcmp(key, "symlinks"))
        return g_strdupv(device->symlinks);
    if (!strcmp(key, "device_size") || !strcmp(key, "setup_by_uid")) {
        g_autofree char *number = g_strdup_printf("%" G_GUINT64_FORMAT,
                                                 !strcmp(key, "device_size") ? device->size : device->uid);
        return one_value(number);
    }
    return NULL;
}

gboolean device_handled(App *app, Device *device)
{
    g_autoptr(Device) parent = NULL;
    for (unsigned depth = 0; device && depth < 64; depth++) {
        gboolean skip;
        yaml_node_t *rule = device_rule_local(app, device, "ignore", &skip);
        if (rule)
            return device->block && !config_bool(rule, FALSE);
        if (!skip) {
            if (!device->block || device->ignored)
                return FALSE;
            if (device->loop_file &&
                (g_str_has_prefix(device->loop_file, "/var/lib/snapd/snaps/") ||
                 g_str_has_prefix(device->loop_file, "/var/lib/extensions/") ||
                 g_str_has_prefix(device->loop_file, "/var/lib/confexts/")))
                return FALSE;
            for (char **link = device->symlinks; *link; link++)
                if (g_str_has_prefix(*link, "/dev/mapper/docker-") ||
                    g_str_has_prefix(*link, "/dev/disk/by-id/dm-name-docker-"))
                    return FALSE;
            if (device->loop_file && g_path_is_absolute(device->loop_file))
                return TRUE;
        }
        Device *next = device_get(app, device_parent(device));
        if (!next)
            return device->block && (skip || !device->system);
        g_clear_pointer(&parent, device_free);
        device = parent = next;
    }
    return FALSE;
}

gboolean device_auto(App *app, Device *device)
{
    return device_handled(app, device) &&
           config_bool(device_rule(app, device, "automount"), TRUE);
}
