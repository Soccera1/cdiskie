#ifndef CDISKIE_H
#define CDISKIE_H

#include <gio/gio.h>
#include <yaml.h>

#define BUS_NAME "org.freedesktop.UDisks2"
#define ROOT_PATH "/org/freedesktop/UDisks2"
#define IFACE(name) BUS_NAME "." name
#define VERSION "0.1.0"

typedef enum { DAEMON, MOUNT, UMOUNT, INFO } Mode;
typedef enum {
    LOCK = 1, FORCE = 2, EJECT = 4, DETACH = 8, RECURSIVE = 16,
    AUTOMATIC = 32, KEEP_LOOP = 64, SETUP_ONLY = 128
} Flags;

typedef struct {
    yaml_document_t document;
    gboolean loaded;
    gboolean automount, notify, indicator, flat, quiet, verbose;
    gboolean indicator_set, checkbox_workaround, menu_workaround;
    gboolean prompt_disabled;
    int tray;
    double cache_minutes;
    char **prompt, **hook, **browser, **terminal;
    char *mount_options;
} Config;

typedef struct {
    GDBusObjectManager *manager;
    GMainLoop *loop;
    Config config;
    GHashTable *passwords;
    gpointer desktop, dialog;
    Mode mode;
    gboolean busy, prompting, stopping;
    GCancellable *cancel;
} App;

typedef struct {
    GDBusObject *object;
    char *path, *file, *drive, *backing, *table, *cleartext, *loop_file;
    char *label, *uuid, *type, *usage, *id, *presentation, *model, *vendor;
    char **mounts, **symlinks;
    gboolean block, filesystem, encrypted, partition, partition_table, loop;
    gboolean system, ignored, auto_hint, ejectable, detachable, media, autoclear;
    guint64 size;
    guint32 uid;
} Device;

typedef struct {
    char *data;
    gsize length;
    gboolean binary, remember;
} Secret;

gboolean config_load(Config *config, const char *path, gboolean disabled, GError **error);
void config_clear(Config *config);
yaml_node_t *config_section(Config *config, const char *name);
yaml_node_t *config_get(Config *config, yaml_node_t *map, const char *key);
const char *config_scalar(yaml_node_t *node);
gboolean config_bool(yaml_node_t *node, gboolean fallback);
char **config_list(Config *config, yaml_node_t *node);
char **command_parse(const char *command, GError **error);
char **command_format(char **command, Device *device, const char *event, const char *extra);
gboolean device_match(Device *device, const char *key, char **patterns);
yaml_node_t *device_rule(App *app, Device *device, const char *key);
yaml_node_t *device_rule_local(App *app, Device *device, const char *key, gboolean *skip);

Device *device_get(App *app, const char *path);
void device_free(Device *device);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(Device, device_free)
GPtrArray *devices_get(App *app);
Device *device_find(App *app, const char *target);
char **device_values(Device *device, const char *key);
gboolean device_handled(App *app, Device *device);
gboolean device_auto(App *app, Device *device);
const char *device_parent(Device *device);

GVariant *udisks_call(App *app, const char *path, const char *interface,
                      const char *method, GVariant *parameters, const GVariantType *type,
                      GError **error);
gboolean device_add(App *app, Device *device, Flags flags, GError **error);
gboolean device_remove(App *app, Device *device, Flags flags, GError **error);
gboolean device_delete(App *app, Device *device, GError **error);
Device *loop_setup(App *app, const char *file, GError **error);
void operation_cache_init(App *app);
gboolean operation_cache_expire(gpointer data);
void operation_log(App *app, const char *verb, const char *device, const char *detail);
void operation_failed(App *app, Device *device, GError *error);

void desktop_init(App *app);
void desktop_clear(App *app);
void desktop_refresh(App *app);
void desktop_cancel(App *app);
gboolean desktop_password(App *app, Device *device, Secret *secret, char **keyfile);
void desktop_event(App *app, Device *device, const char *event, const char *detail);
void launch_command(char **argv);

#endif
