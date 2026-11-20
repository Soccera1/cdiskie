#include "cdiskie.h"

#include <getopt.h>
#include <glib-unix.h>
#include <math.h>
#include <signal.h>
#include <string.h>

enum {
    INDICATOR = 256, NO_INDICATOR, CACHE, NO_CACHE, TERMINAL, NO_TERMINAL,
    HOOK, NO_HOOK, CHECKBOX, NO_CHECKBOX, MENU_UPDATE, NO_MENU_UPDATE
};

#define D (1u << DAEMON)
#define M (1u << MOUNT)
#define U (1u << UMOUNT)
#define I (1u << INFO)
#define ALL (D | M | U | I)

typedef struct {
    const char *name;
    int code;
    const char *argument;
    unsigned modes;
    const char *description;
} Option;

static const Option options[] = {
    { "help", 'h', NULL, ALL, "Show this help" },
    { "version", 'V', NULL, ALL, "Show the program version" },
    { "config", 'c', "FILE", ALL, "Read a YAML or JSON configuration file" },
    { "no-config", 'C', NULL, ALL, "Skip configuration" },
    { "verbose", 'v', NULL, ALL, "Log D-Bus operations" },
    { "quiet", 'q', NULL, ALL, "Suppress routine output" },
    { "automount", 'a', NULL, D, "Automatically mount removable media" },
    { "no-automount", 'A', NULL, D, "Disable automatic mounting" },
    { "notify", 'n', NULL, D, "Enable desktop notifications" },
    { "no-notify", 'N', NULL, D, "Disable desktop notifications" },
    { "tray", 't', NULL, D, "Show the tray icon" },
    { "smart-tray", 's', NULL, D, "Show the tray icon when devices have actions" },
    { "no-tray", 'T', NULL, D, "Hide the tray icon" },
    { "menu", 'm', "STYLE", D, "Choose a flat or nested tray menu" },
    { "appindicator", INDICATOR, NULL, D, "Use the AppIndicator tray backend" },
    { "no-appindicator", NO_INDICATOR, NULL, D, "Use the GTK status icon backend" },
    { "file-manager", 'f', "COMMAND", D, "Open mount directories with COMMAND" },
    { "no-file-manager", 'F', NULL, D, "Disable opening mount directories" },
    { "terminal", TERMINAL, "COMMAND", D, "Open terminals with COMMAND" },
    { "no-terminal", NO_TERMINAL, NULL, D, "Disable the terminal action" },
    { "password-prompt", 'p', "COMMAND", D | M, "Read passphrases from COMMAND" },
    { "no-password-prompt", 'P', NULL, D | M, "Disable passphrase prompting" },
    { "password-cache", CACHE, "MINUTES", D, "Cache successful passphrases in memory" },
    { "no-password-cache", NO_CACHE, NULL, D, "Disable passphrase caching" },
    { "event-hook", HOOK, "COMMAND", D, "Run COMMAND on device events" },
    { "no-event-hook", NO_HOOK, NULL, D, "Disable the event hook" },
    { "notify-command", HOOK, "COMMAND", D, NULL },
    { "no-notify-command", NO_HOOK, NULL, D, NULL },
    { "menu-checkbox-workaround", CHECKBOX, NULL, D, "Use plain menu items for toggles" },
    { "no-menu-checkbox-workaround", NO_CHECKBOX, NULL, D, "Use checkboxes for menu toggles" },
    { "menu-update-workaround", MENU_UPDATE, NULL, D, "Keep the indicator menu widget on updates" },
    { "no-menu-update-workaround", NO_MENU_UPDATE, NULL, D, "Replace the indicator menu on updates" },
    { "all", 'a', NULL, M | U | I, "Act on every eligible device" },
    { "recursive", 'r', NULL, M, "Mount child partitions and unlocked filesystems" },
    { "no-recursive", 'R', NULL, M, "Operate on the named device only" },
    { "options", 'o', "OPTIONS", M, "Pass comma-separated mount options" },
    { "detach", 'd', NULL, U, "Power off the drive or detach the loop device" },
    { "no-detach", 'D', NULL, U, "Keep the drive or loop device attached" },
    { "eject", 'e', NULL, U, "Eject media from the drive" },
    { "no-eject", 'E', NULL, U, "Keep media in the drive" },
    { "force", 'f', NULL, U, "Recursively unmount child devices" },
    { "no-force", 'F', NULL, U, "Operate on the named device only" },
    { "lock", 'l', NULL, U, "Lock the backing encrypted volume" },
    { "no-lock", 'L', NULL, U, "Keep the backing encrypted volume unlocked" },
    { "output", 'o', "FORMAT", I, "Print a device attribute or format string" },
    { "filter", 'f', "FILTER", I, "Filter by attribute, !attribute, or attribute=value" },
};

typedef struct {
    const char *config_path;
    gboolean no_config, all, help, version;
    Flags flags;
    const char *output;
    GPtrArray *filters;
} Invocation;

static void print_help(const char *name, Mode mode)
{
    g_print("Usage: %s [OPTIONS]%s\n\n", name, mode == DAEMON ? "" : " (-a | DEVICE...)");
    for (gsize i = 0; i < G_N_ELEMENTS(options); i++) {
        const Option *option = &options[i];
        if (!(option->modes & (1u << mode)) || !option->description)
            continue;
        g_autofree char *short_name = option->code < 256 ? g_strdup_printf("-%c, ", option->code) :
                                                           g_strdup("    ");
        g_autofree char *label = g_strconcat(short_name, "--", option->name,
            option->argument ? "=" : "", option->argument, NULL);
        g_print("  %-32s %s\n", label, option->description);
    }
}

static gboolean replace_command(char ***command, const char *value, GError **error)
{
    g_clear_pointer(command, g_strfreev);
    *command = command_parse(value, error);
    return !*error;
}

static gboolean parse_options(App *app, Invocation *invocation, int argc, char **argv, GError **error)
{
    struct option longs[G_N_ELEMENTS(options) + 1] = { 0 };
    g_autoptr(GString) shorts = g_string_new("");
    unsigned count = 0;
    for (gsize i = 0; i < G_N_ELEMENTS(options); i++) {
        const Option *option = &options[i];
        if (!(option->modes & (1u << app->mode)))
            continue;
        longs[count++] = (struct option) {
            option->name, option->argument ? required_argument : no_argument, NULL, option->code
        };
        if (option->code < 256) {
            g_string_append_c(shorts, option->code);
            if (option->argument)
                g_string_append_c(shorts, ':');
        }
    }

    /* Resolve the config file first so every CLI option can override its value. */
    opterr = 0;
    int option;
    while ((option = getopt_long(argc, argv, shorts->str, longs, NULL)) != -1) {
        switch (option) {
        case 'c': invocation->config_path = optarg; break;
        case 'C': invocation->no_config = TRUE; break;
        case 'h': invocation->help = TRUE; break;
        case 'V': invocation->version = TRUE; break;
        case '?':
            g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                        "Invalid option or missing argument near %s", argv[optind - 1]);
            return FALSE;
        default: break;
        }
    }
    if (invocation->help || invocation->version)
        return TRUE;
    if (!config_load(&app->config, invocation->config_path, invocation->no_config, error))
        return FALSE;

    Config *config = &app->config;
    optind = 1;
    while ((option = getopt_long(argc, argv, shorts->str, longs, NULL)) != -1) {
        switch (option) {
        case 'a':
            if (app->mode == DAEMON)
                config->automount = TRUE;
            else
                invocation->all = TRUE;
            break;
        case 'A': config->automount = FALSE; break;
        case 'n': config->notify = TRUE; break;
        case 'N': config->notify = FALSE; break;
        case 's': config->tray = 2; break;
        case 't': config->tray = 1; break;
        case 'T': config->tray = 0; break;
        case 'v': config->verbose = TRUE; config->quiet = FALSE; break;
        case 'q': config->quiet = TRUE; config->verbose = FALSE; break;
        case 'm':
            if (strcmp(optarg, "flat") && strcmp(optarg, "nested") && strcmp(optarg, "smart")) {
                g_set_error_literal(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                                    "Menu style must be flat or nested");
                return FALSE;
            }
            config->flat = !strcmp(optarg, "flat");
            break;
        case 'f':
            if (app->mode == UMOUNT)
                invocation->flags |= FORCE;
            else if (app->mode == INFO)
                g_ptr_array_add(invocation->filters, optarg);
            else if (!replace_command(&config->browser, optarg, error))
                return FALSE;
            break;
        case 'F':
            if (app->mode == UMOUNT)
                invocation->flags &= ~FORCE;
            else
                g_clear_pointer(&config->browser, g_strfreev);
            break;
        case 'p':
            if (!replace_command(&config->prompt, optarg, error))
                return FALSE;
            config->prompt_disabled = !config->prompt;
            break;
        case 'P': config->prompt_disabled = TRUE; break;
        case 'r': invocation->flags = (invocation->flags & ~SETUP_ONLY) | RECURSIVE; break;
        case 'R': invocation->flags = (invocation->flags & ~RECURSIVE) | SETUP_ONLY; break;
        case 'o':
            if (app->mode == INFO)
                invocation->output = optarg;
            else {
                g_free(config->mount_options);
                config->mount_options = g_strdup(optarg);
            }
            break;
        case 'd': invocation->flags |= DETACH; break;
        case 'D': invocation->flags = (invocation->flags & ~DETACH) | KEEP_LOOP; break;
        case 'e': invocation->flags |= EJECT; break;
        case 'E': invocation->flags &= ~EJECT; break;
        case 'l': invocation->flags |= LOCK; break;
        case 'L': invocation->flags &= ~LOCK; break;
        case INDICATOR: config->indicator = TRUE; config->indicator_set = TRUE; break;
        case NO_INDICATOR: config->indicator = FALSE; config->indicator_set = TRUE; break;
        case CACHE: {
            char *end;
            config->cache_minutes = g_ascii_strtod(optarg, &end);
            if (!*optarg || *end || !isfinite(config->cache_minutes) || config->cache_minutes < 0) {
                g_set_error_literal(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                                    "Password cache timeout must be a nonnegative number");
                return FALSE;
            }
            break;
        }
        case NO_CACHE: config->cache_minutes = 0; break;
        case TERMINAL:
            if (!replace_command(&config->terminal, optarg, error))
                return FALSE;
            break;
        case NO_TERMINAL: g_clear_pointer(&config->terminal, g_strfreev); break;
        case HOOK:
            if (!replace_command(&config->hook, optarg, error))
                return FALSE;
            break;
        case NO_HOOK: g_clear_pointer(&config->hook, g_strfreev); break;
        case CHECKBOX: config->checkbox_workaround = TRUE; break;
        case NO_CHECKBOX: config->checkbox_workaround = FALSE; break;
        case MENU_UPDATE: config->menu_workaround = TRUE; break;
        case NO_MENU_UPDATE: config->menu_workaround = FALSE; break;
        default: break;
        }
    }
    gboolean targets = optind < argc;
    if ((app->mode == DAEMON && targets) || (app->mode != DAEMON && targets == invocation->all)) {
        g_set_error_literal(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
            app->mode == DAEMON ? "Unexpected positional arguments" : "Specify --all or one or more devices");
        return FALSE;
    }
    return TRUE;
}

static void print_info(Device *device, Invocation *invocation)
{
    for (guint i = 0; i < invocation->filters->len; i++) {
        const char *filter = invocation->filters->pdata[i];
        const char *equals = strchr(filter, '=');
        const char *key = !equals && *filter == '!' ? filter + 1 : filter;
        g_autofree char *attribute = equals ? g_strndup(key, equals - key) : g_strdup(key);
        const char *pattern = equals ? equals + 1 : *filter == '!' ? "false" : "true";
        char *patterns[] = { (char *)pattern, NULL };
        if (!device_match(device, attribute, patterns))
            return;
    }
    const char *output = invocation->output ? invocation->output : "device_presentation";
    g_auto(GStrv) values = device_values(device, output);
    if (values) {
        g_autofree char *value = g_strjoinv(" ", values);
        g_print("%s\n", value);
    } else {
        char *format[] = { (char *)output, NULL };
        if (*output) {
            g_auto(GStrv) result = command_format(format, device, NULL, NULL);
            g_print("%s\n", result[0]);
        } else {
            g_print("%s\n", device->presentation);
        }
    }
}

static gboolean run_device(App *app, Device *device, Invocation *invocation, Flags flags)
{
    if (app->mode == INFO) {
        print_info(device, invocation);
        return TRUE;
    }
    g_autoptr(GError) error = NULL;
    gboolean success = app->mode == MOUNT ? device_add(app, device, flags, &error) :
                                            device_remove(app, device, flags, &error);
    if (!success)
        operation_failed(app, device, error);
    return success;
}

static int run_cli(App *app, Invocation *invocation, char **targets)
{
    gboolean success = TRUE;
    if (invocation->all) {
        g_autoptr(GPtrArray) devices = devices_get(app);
        for (guint i = 0; i < devices->len && !app->stopping; i++) {
            Device *device = devices->pdata[i];
            if (!device_handled(app, device))
                continue;
            if (app->mode == UMOUNT) {
                g_autoptr(Device) parent = device_get(app, device_parent(device));
                if (parent && device_handled(app, parent))
                    continue;
            }
            if (app->mode == MOUNT && (device->partition_table || device->cleartext))
                continue;
            Flags flags = invocation->flags | AUTOMATIC;
            if (app->mode == UMOUNT)
                flags |= FORCE;
            if (!run_device(app, device, invocation, flags))
                success = FALSE;
        }
    } else {
        for (; *targets && !app->stopping; targets++) {
            g_autoptr(Device) device = device_find(app, *targets);
            if (!device && app->mode == MOUNT && g_file_test(*targets, G_FILE_TEST_IS_REGULAR)) {
                g_autoptr(GError) error = NULL;
                device = loop_setup(app, *targets, &error);
                if (!device) {
                    g_printerr("cdiskie: %s: %s\n", *targets, error->message);
                    success = FALSE;
                    continue;
                }
                if (invocation->flags & SETUP_ONLY)
                    continue;
            }
            if (!device) {
                g_printerr("cdiskie: %s: no matching device\n", *targets);
                success = FALSE;
            } else if (!run_device(app, device, invocation, invocation->flags)) {
                success = FALSE;
            }
        }
    }
    return app->stopping ? 130 : success ? 0 : 1;
}

typedef struct {
    App *app;
    GHashTable *previous;
    GHashTable *attempted;
    GHashTable *changed;
    guint pending;
    guint automount;
    gboolean initial, deferred;
} Monitor;

static gboolean scan_devices(gpointer data);

static void schedule_scan(Monitor *monitor)
{
    if (!monitor->pending && !monitor->app->stopping)
        monitor->pending = g_timeout_add(120, scan_devices, monitor);
}

static void report_changes(Monitor *monitor, GHashTable *current)
{
    App *app = monitor->app;
    GHashTableIter iter;
    gpointer path, value;
    g_hash_table_iter_init(&iter, monitor->previous);
    while (g_hash_table_iter_next(&iter, &path, &value)) {
        Device *old = value;
        if (!g_hash_table_contains(current, path)) {
            g_hash_table_remove(monitor->attempted, path);
            desktop_event(app, old, "device_removed", NULL);
        }
    }
    g_hash_table_iter_init(&iter, current);
    while (g_hash_table_iter_next(&iter, &path, &value)) {
        Device *device = value;
        Device *old = g_hash_table_lookup(monitor->previous, path);
        if (!old) {
            desktop_event(app, device, "device_added", NULL);
            continue;
        }
        if (!!old->mounts[0] != !!device->mounts[0])
            desktop_event(app, device, device->mounts[0] ? "device_mounted" : "device_unmounted",
                          device->mounts[0] ? device->mounts[0] : old->mounts[0]);
        if (!!old->cleartext != !!device->cleartext)
            desktop_event(app, device, device->cleartext ? "device_unlocked" : "device_locked", NULL);
        if (old->media != device->media) {
            g_hash_table_remove(monitor->attempted, path);
            desktop_event(app, device, device->media ? "media_added" : "media_removed", NULL);
        }
        if (g_hash_table_contains(monitor->changed, path))
            desktop_event(app, device, "device_changed", NULL);
    }
}

static gboolean automount_devices(gpointer data)
{
    Monitor *monitor = data;
    App *app = monitor->app;
    if (app->stopping || !app->config.automount) {
        monitor->automount = 0;
        return G_SOURCE_REMOVE;
    }
    if (app->busy && !app->prompting)
        return G_SOURCE_CONTINUE;
    monitor->automount = 0;
    g_autoptr(GPtrArray) devices = devices_get(app);
    gboolean busy = app->busy;
    gboolean prompting = app->prompting;
    app->prompting = FALSE;
    app->busy = TRUE;
    /* Plain filesystem arrivals can proceed even while a passphrase dialog is open. */
    for (int encrypted = 0; encrypted <= !busy; encrypted++) {
        for (guint i = 0; i < devices->len && !app->stopping && app->config.automount; i++) {
            Device *candidate = devices->pdata[i];
            if (candidate->encrypted != encrypted ||
                (!candidate->filesystem && !candidate->encrypted))
                continue;
            g_autoptr(Device) device = device_get(app, candidate->path);
            if (!device || !device->media || !device_auto(app, device))
                continue;
            if (!g_hash_table_add(monitor->attempted, g_strdup(device->path)))
                continue;
            if (device->mounts[0] || device->cleartext)
                continue;
            g_autoptr(GError) error = NULL;
            if (!device_add(app, device, AUTOMATIC, &error))
                operation_failed(app, device, error);
        }
    }
    app->busy = busy;
    app->prompting = prompting;
    if (busy) {
        monitor->deferred = TRUE;
    } else if (monitor->deferred) {
        monitor->deferred = FALSE;
        schedule_scan(monitor);
    }
    return G_SOURCE_REMOVE;
}

static gboolean scan_devices(gpointer data)
{
    Monitor *monitor = data;
    App *app = monitor->app;
    monitor->pending = 0;
    g_autoptr(GPtrArray) devices = devices_get(app);
    GHashTable *current = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                               (GDestroyNotify)device_free);
    for (guint i = 0; i < devices->len; i++) {
        Device *device = devices->pdata[i];
        if (device_handled(app, device))
            g_hash_table_insert(current, g_strdup(device->path), device_get(app, device->path));
    }
    if (!monitor->initial)
        report_changes(monitor, current);
    g_hash_table_remove_all(monitor->changed);
    monitor->initial = FALSE;
    g_hash_table_unref(monitor->previous);
    monitor->previous = current;
    desktop_refresh(app);

    if (app->config.automount && !monitor->automount)
        monitor->automount = g_timeout_add(15, automount_devices, monitor);
    return G_SOURCE_REMOVE;
}

static void object_changed(GDBusObjectManager *manager, GDBusObject *object, gpointer data)
{
    (void)manager;
    (void)object;
    schedule_scan(data);
}

static void object_removed(GDBusObjectManager *manager, GDBusObject *object, gpointer data)
{
    Monitor *monitor = data;
    g_hash_table_remove(monitor->attempted, g_dbus_object_get_object_path(object));
    object_changed(manager, object, data);
}

static void interface_changed(GDBusObjectManager *manager, GDBusObject *object,
                              GDBusInterface *interface, gpointer data)
{
    (void)interface;
    Monitor *monitor = data;
    g_hash_table_add(monitor->changed, g_strdup(g_dbus_object_get_object_path(object)));
    object_changed(manager, object, data);
}

static void properties_changed(GDBusObjectManager *manager, GDBusObject *object, GDBusProxy *proxy,
                               GVariant *changes, const char *const *invalidated, gpointer data)
{
    (void)changes;
    (void)invalidated;
    interface_changed(manager, object, G_DBUS_INTERFACE(proxy), data);
}

static void job_signal(GDBusObjectManagerClient *manager, GDBusObjectProxy *object,
                       GDBusProxy *proxy, const char *sender, const char *signal,
                       GVariant *parameters, gpointer data)
{
    (void)manager;
    (void)object;
    (void)sender;
    Monitor *monitor = data;
    App *app = monitor->app;
    if (app->busy || strcmp(signal, "Completed") ||
        strcmp(g_dbus_proxy_get_interface_name(proxy), IFACE("Job")) ||
        !g_variant_is_of_type(parameters, G_VARIANT_TYPE("(bs)")))
        return;
    gboolean success;
    const char *message;
    g_variant_get(parameters, "(b&s)", &success, &message);
    if (success)
        return;
    g_autoptr(GVariant) paths = g_dbus_proxy_get_cached_property(proxy, "Objects");
    if (!paths)
        return;
    GVariantIter iter;
    g_variant_iter_init(&iter, paths);
    const char *path;
    while (g_variant_iter_next(&iter, "&o", &path)) {
        g_autoptr(Device) device = device_get(app, path);
        if (device && device_handled(app, device))
            desktop_event(app, device, "job_failed", message);
    }
}

static gboolean stop_daemon(gpointer data)
{
    App *app = data;
    app->stopping = TRUE;
    g_cancellable_cancel(app->cancel);
    desktop_cancel(app);
    if (app->loop)
        g_main_loop_quit(app->loop);
    return G_SOURCE_CONTINUE;
}

static void run_daemon(App *app)
{
    app->loop = g_main_loop_new(NULL, FALSE);
    Monitor monitor = {
        .app = app,
        .previous = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)device_free),
        .attempted = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL),
        .changed = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL),
        .initial = TRUE,
    };
    desktop_init(app);
    g_signal_connect(app->manager, "object-added", G_CALLBACK(object_changed), &monitor);
    g_signal_connect(app->manager, "object-removed", G_CALLBACK(object_removed), &monitor);
    g_signal_connect(app->manager, "interface-added", G_CALLBACK(interface_changed), &monitor);
    g_signal_connect(app->manager, "interface-removed", G_CALLBACK(interface_changed), &monitor);
    g_signal_connect(app->manager, "interface-proxy-properties-changed",
                     G_CALLBACK(properties_changed), &monitor);
    g_signal_connect(app->manager, "interface-proxy-signal", G_CALLBACK(job_signal), &monitor);
    guint expire = g_timeout_add_seconds(1, operation_cache_expire, app);
    schedule_scan(&monitor);
    g_main_loop_run(app->loop);
    g_signal_handlers_disconnect_by_data(app->manager, &monitor);
    if (monitor.pending)
        g_source_remove(monitor.pending);
    if (monitor.automount)
        g_source_remove(monitor.automount);
    g_source_remove(expire);
    g_hash_table_unref(monitor.attempted);
    g_hash_table_unref(monitor.changed);
    g_hash_table_unref(monitor.previous);
    desktop_clear(app);
    g_clear_pointer(&app->loop, g_main_loop_unref);
}

int main(int argc, char **argv)
{
    g_autofree char *name = g_path_get_basename(argv[0]);
    g_set_prgname(name);
    App app = { .mode = DAEMON };
    if (g_str_has_suffix(name, "-mount"))
        app.mode = MOUNT;
    else if (g_str_has_suffix(name, "-umount"))
        app.mode = UMOUNT;
    else if (g_str_has_suffix(name, "-info"))
        app.mode = INFO;
    g_autoptr(GPtrArray) filters = g_ptr_array_new();
    Invocation invocation = { .flags = app.mode == UMOUNT ? LOCK : 0, .filters = filters };
    g_autoptr(GError) error = NULL;
    int status = 0;
    if (!parse_options(&app, &invocation, argc, argv, &error)) {
        g_printerr("%s: %s\n", name, error->message);
        status = 2;
    } else if (invocation.help) {
        print_help(name, app.mode);
    } else if (invocation.version) {
        g_print("cdiskie %s\n", VERSION);
    } else {
        app.manager = g_dbus_object_manager_client_new_for_bus_sync(G_BUS_TYPE_SYSTEM,
            G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE, BUS_NAME, ROOT_PATH, NULL, NULL, NULL, NULL, &error);
        g_autofree char *owner = app.manager ?
            g_dbus_object_manager_client_get_name_owner(G_DBUS_OBJECT_MANAGER_CLIENT(app.manager)) : NULL;
        if (!owner) {
            g_printerr("%s: cannot connect to UDisks2%s%s\n", name,
                       error ? ": " : "", error ? error->message : "");
            status = 1;
        } else {
            operation_cache_init(&app);
            app.cancel = g_cancellable_new();
            guint interrupt = g_unix_signal_add(SIGINT, stop_daemon, &app);
            guint terminate = g_unix_signal_add(SIGTERM, stop_daemon, &app);
            if (app.mode == DAEMON)
                run_daemon(&app);
            else
                status = run_cli(&app, &invocation, argv + optind);
            g_source_remove(terminate);
            g_source_remove(interrupt);
            g_object_unref(app.cancel);
            g_hash_table_unref(app.passwords);
        }
        g_clear_object(&app.manager);
    }
    config_clear(&app.config);
    return status;
}
