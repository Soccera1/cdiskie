#include "cdiskie.h"

#include <errno.h>
#include <fcntl.h>
#include <gio/gunixfdlist.h>
#include <gio/gunixinputstream.h>
#include <glib/gstdio.h>
#include <math.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

typedef struct {
    GVariant *reply;
    GError *error;
    gboolean done;
} Call;

static void call_ready(GObject *source, GAsyncResult *result, gpointer data)
{
    Call *call = data;
    call->reply = g_dbus_connection_call_with_unix_fd_list_finish(
        G_DBUS_CONNECTION(source), NULL, result, &call->error);
    call->done = TRUE;
}

static GVariant *call_with_fds(App *app, const char *path, const char *interface,
                               const char *method, GVariant *parameters, const GVariantType *type,
                               GUnixFDList *fds, GError **error)
{
    GDBusObjectManagerClient *manager = G_DBUS_OBJECT_MANAGER_CLIENT(app->manager);
    GDBusConnection *bus = g_dbus_object_manager_client_get_connection(manager);
    g_autofree char *owner = g_dbus_object_manager_client_get_name_owner(manager);
    if (app->config.verbose)
        g_printerr("%s.%s %s\n", interface, method, path);
    Call call = { 0 };
    g_dbus_connection_call_with_unix_fd_list(
        bus, owner ? owner : BUS_NAME, path, interface, method, parameters, type,
        G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION, 120000, fds, app->cancel, call_ready, &call);
    while (!call.done)
        g_main_context_iteration(NULL, TRUE);
    if (call.error) {
        g_dbus_error_strip_remote_error(call.error);
        g_propagate_error(error, call.error);
    }
    return call.reply;
}

GVariant *udisks_call(App *app, const char *path, const char *interface,
                      const char *method, GVariant *parameters, const GVariantType *type,
                      GError **error)
{
    return call_with_fds(app, path, interface, method, parameters, type, NULL, error);
}

void operation_log(App *app, const char *verb, const char *device, const char *detail)
{
    if (!app->config.quiet)
        g_print("%s %s%s%s\n", verb, device, detail ? " on " : "", detail ? detail : "");
}

void operation_failed(App *app, Device *device, GError *error)
{
    if (!error || g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;
    g_printerr("cdiskie: %s: %s\n", device->presentation, error->message);
    desktop_event(app, device, "job_failed", error->message);
}

static gboolean fail(GError **error, GIOErrorEnum code, const char *message)
{
    g_set_error_literal(error, G_IO_ERROR, code, message);
    return FALSE;
}

static GVariant *empty_options(void)
{
    return g_variant_new_array(G_VARIANT_TYPE("{sv}"), NULL, 0);
}

static gboolean simple_call(App *app, const char *path, const char *interface,
                            const char *method, GVariant *options, GError **error)
{
    g_autoptr(GVariant) reply = udisks_call(app, path, interface, method,
        g_variant_new("(@a{sv})", options ? options : empty_options()), G_VARIANT_TYPE_UNIT, error);
    return reply != NULL;
}

static gboolean mount_fs(App *app, Device *device, GError **error)
{
    if (device->mounts[0])
        return TRUE;
    g_auto(GStrv) list = config_list(&app->config, device_rule(app, device, "options"));
    g_autofree char *joined = g_strjoinv(",", list);
    const char *options = app->config.mount_options ? app->config.mount_options : joined;
    g_auto(GStrv) fields = g_strsplit(options, ",", -1);
    for (char **field = fields; *field; field++)
        g_strstrip(*field);
    g_autofree char *normalized = g_strjoinv(",", fields);
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
    if (*normalized)
        g_variant_builder_add(&builder, "{sv}", "options", g_variant_new_string(normalized));
    g_autoptr(GVariant) reply = udisks_call(app, device->path, IFACE("Filesystem"), "Mount",
        g_variant_new("(a{sv})", &builder), G_VARIANT_TYPE("(s)"), error);
    if (!reply)
        return FALSE;
    const char *path;
    g_variant_get(reply, "(&s)", &path);
    operation_log(app, "mounted", device->presentation, path);
    return TRUE;
}

typedef struct {
    Secret secret;
    gint64 expires;
} Password;

static void wipe(char *text, gsize length)
{
    volatile char *byte = text;
    while (length--)
        *byte++ = 0;
}

static void secret_clear(Secret *secret)
{
    if (secret->data)
        wipe(secret->data, secret->length);
    g_free(secret->data);
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(Secret, secret_clear)

static void password_free(gpointer data)
{
    Password *password = data;
    secret_clear(&password->secret);
    g_free(password);
}

void operation_cache_init(App *app)
{
    app->passwords = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, password_free);
}

static gboolean expire_password(gpointer key, gpointer value, gpointer now)
{
    (void)key;
    return ((Password *)value)->expires <= *(gint64 *)now;
}

gboolean operation_cache_expire(gpointer data)
{
    App *app = data;
    gint64 now = g_get_monotonic_time();
    g_hash_table_foreach_remove(app->passwords, expire_password, &now);
    return G_SOURCE_CONTINUE;
}

typedef struct {
    gboolean done;
    char *line;
    gsize length;
    GError *error;
} TerminalInput;

static void terminal_ready(GObject *source, GAsyncResult *result, gpointer data)
{
    TerminalInput *input = data;
    input->line = g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(source), result,
                                                      &input->length, &input->error);
    input->done = TRUE;
}

static gboolean tty_password(App *app, Device *device, Secret *secret, GError **error)
{
    int fd = g_open("/dev/tty", O_RDWR | O_CLOEXEC, 0);
    if (fd < 0)
        return fail(error, G_IO_ERROR_NOT_SUPPORTED, "No terminal is available for the passphrase");
    g_autoptr(GInputStream) tty = g_unix_input_stream_new(fd, TRUE);
    struct termios settings;
    if (tcgetattr(fd, &settings) < 0)
        return fail(error, G_IO_ERROR_FAILED, "Cannot read terminal settings");
    struct termios hidden = settings;
    hidden.c_lflag &= ~ECHO;
    if (tcsetattr(fd, TCSAFLUSH, &hidden) < 0)
        return fail(error, G_IO_ERROR_FAILED, "Cannot disable terminal echo");
    dprintf(fd, "Passphrase for %s: ", device->presentation);
    g_autoptr(GDataInputStream) input_stream = g_data_input_stream_new(tty);
    TerminalInput input = { 0 };
    g_data_input_stream_read_line_async(input_stream, G_PRIORITY_DEFAULT, app->cancel,
                                        terminal_ready, &input);
    while (!input.done)
        g_main_context_iteration(NULL, TRUE);
    tcsetattr(fd, TCSANOW, &settings);
    dprintf(fd, "\n");
    if (input.error) {
        g_propagate_error(error, input.error);
        return FALSE;
    }
    if (!input.line)
        return fail(error, G_IO_ERROR_CANCELLED, "Passphrase entry cancelled");
    secret->data = input.line;
    secret->length = input.length;
    return TRUE;
}

typedef struct {
    gboolean done, success;
    GBytes *output;
    GError *error;
} PromptResult;

static void prompt_ready(GObject *source, GAsyncResult *result, gpointer data)
{
    PromptResult *prompt = data;
    prompt->success = g_subprocess_communicate_finish(G_SUBPROCESS(source), result,
        &prompt->output, NULL, &prompt->error);
    prompt->done = TRUE;
}

static gboolean get_password(App *app, Device *device, Secret *secret, char **keyfile, GError **error)
{
    char **command = app->config.prompt;
    const char *program = command ? command[0] : NULL;
    if (!program)
        program = app->mode == DAEMON ? "builtin:gui" : "builtin:tty";
    secret->remember = app->config.cache_minutes > 0;
    if (!strcmp(program, "builtin:tty") || !strcmp(program, "builtin:gui")) {
        gboolean tty = isatty(STDIN_FILENO);
#ifdef WITH_DESKTOP
        gboolean display = g_getenv("DISPLAY") || g_getenv("WAYLAND_DISPLAY");
#else
        gboolean display = FALSE;
#endif
        if (display && (!tty || !strcmp(program, "builtin:gui"))) {
            if (desktop_password(app, device, secret, keyfile))
                return TRUE;
            return fail(error, G_IO_ERROR_CANCELLED, "Graphical passphrase entry cancelled");
        }
        if (!tty && app->mode == DAEMON)
            return fail(error, G_IO_ERROR_CANCELLED, "No passphrase prompt is available");
        return tty_password(app, device, secret, error);
    }
    g_auto(GStrv) argv = command_format(command, device, NULL, NULL);
    g_autoptr(GSubprocess) process = g_subprocess_newv((const char *const *)argv,
        G_SUBPROCESS_FLAGS_STDOUT_PIPE, error);
    if (!process)
        return FALSE;
    PromptResult result = { 0 };
    g_subprocess_communicate_async(process, NULL, app->cancel, prompt_ready, &result);
    while (!result.done)
        g_main_context_iteration(NULL, TRUE);
    if (!result.success || !g_subprocess_get_successful(process)) {
        if (g_error_matches(result.error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            g_subprocess_force_exit(process);
        g_clear_pointer(&result.output, g_bytes_unref);
        if (result.error)
            g_propagate_error(error, result.error);
        else
            fail(error, G_IO_ERROR_CANCELLED, "Passphrase command did not succeed");
        return FALSE;
    }
    gsize length;
    char *output = g_bytes_unref_to_data(result.output, &length);
    secret->binary = length && !g_utf8_validate(output, length, NULL);
    secret->length = length;
    secret->data = g_new(char, length + 1);
    if (length) {
        memcpy(secret->data, output, length);
        wipe(output, length);
    }
    g_free(output);
    if (!secret->binary && length && secret->data[length - 1] == '\n')
        secret->length--;
    secret->data[secret->length] = '\0';
    return TRUE;
}

static char *unlock_call(App *app, Device *device, const char *password,
                         const char *key, gsize key_length, GError **error)
{
    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
    if (key)
        g_variant_builder_add(&options, "{sv}", "keyfile_contents",
            g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, key, key_length, 1));
    g_autoptr(GVariant) reply = udisks_call(app, device->path, IFACE("Encrypted"), "Unlock",
        g_variant_new("(sa{sv})", password, &options), G_VARIANT_TYPE("(o)"), error);
    char *path = NULL;
    if (reply)
        g_variant_get(reply, "(o)", &path);
    return path;
}

static char *unlock_secret(App *app, Device *device, Secret *secret, GError **error)
{
    if (secret->binary || !secret->length || !g_utf8_validate(secret->data, secret->length, NULL))
        return unlock_call(app, device, "", secret->data, secret->length, error);
    return unlock_call(app, device, secret->data, NULL, 0, error);
}

static gboolean expand_variable(const GMatchInfo *match, GString *result, gpointer data)
{
    (void)data;
    g_autofree char *name = g_match_info_fetch(match, 1);
    if (!*name) {
        g_free(name);
        name = g_match_info_fetch(match, 2);
    }
    const char *value = g_getenv(name);
    g_autofree char *literal = value ? NULL : g_match_info_fetch(match, 0);
    g_string_append(result, value ? value : literal);
    return FALSE;
}

static char *expand_keyfile(const char *keyfile)
{
    g_autoptr(GRegex) variables = g_regex_new("\\$([A-Za-z_][A-Za-z_0-9]*)|\\$\\{([^}]+)\\}",
                                               0, 0, NULL);
    g_autofree char *path = g_regex_replace_eval(variables, keyfile, -1, 0, 0,
                                                  expand_variable, NULL, NULL);
    if (g_str_has_prefix(path, "~/"))
        return g_build_filename(g_get_home_dir(), path + 2, NULL);
    return g_steal_pointer(&path);
}

static char *unlock(App *app, Device *device, GError **error)
{
    if (device->cleartext)
        return g_strdup(device->cleartext);
    const char *cache_key = device->uuid ? device->uuid : device->id;
    operation_cache_expire(app);
    Password *cached = cache_key ? g_hash_table_lookup(app->passwords, cache_key) : NULL;
    if (cached) {
        char *path = unlock_secret(app, device, &cached->secret, NULL);
        if (path)
            return path;
        if (app->stopping) {
            fail(error, G_IO_ERROR_CANCELLED, "Passphrase retrieval cancelled");
            return NULL;
        }
        g_hash_table_remove(app->passwords, cache_key);
    }
    const char *keyfile = config_scalar(device_rule(app, device, "keyfile"));
    g_autofree char *selected = keyfile && *keyfile ? expand_keyfile(keyfile) : NULL;
    g_auto(Secret) secret = { 0 };
    if (!selected && app->config.prompt_disabled) {
        fail(error, G_IO_ERROR_NOT_SUPPORTED, "Passphrase prompting is disabled");
        return NULL;
    }
    if (!selected) {
        app->prompting = TRUE;
        gboolean obtained = get_password(app, device, &secret, &selected, error);
        app->prompting = FALSE;
        if (!obtained)
            return NULL;
    }
    if (selected) {
        if (!g_file_get_contents(selected, &secret.data, &secret.length, error))
            return NULL;
        secret.binary = TRUE;
    }
    char *path = unlock_secret(app, device, &secret, error);
    double minutes = app->config.cache_minutes;
    if (path && cache_key && secret.remember && minutes > 0 && isfinite(minutes)) {
        Password *entry = g_new(Password, 1);
        entry->secret = secret;
        secret.data = NULL;
        entry->expires = g_get_monotonic_time() + MIN(minutes, 1e8) * 60 * G_USEC_PER_SEC;
        g_hash_table_replace(app->passwords, g_strdup(cache_key), entry);
    }
    return path;
}

static gboolean wake(gpointer data)
{
    (void)data;
    return G_SOURCE_CONTINUE;
}

static Device *wait_device(App *app, const char *path, GError **error)
{
    gint64 deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
    guint timer = g_timeout_add(25, wake, NULL);
    Device *device;
    while (!(device = device_get(app, path)) && !app->stopping &&
           g_get_monotonic_time() < deadline)
        g_main_context_iteration(NULL, TRUE);
    g_source_remove(timer);
    if (!device)
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Device %s did not appear", path);
    return device;
}

static gboolean add_children(App *app, Device *device, Flags flags, GError **error)
{
    g_autoptr(GPtrArray) all = devices_get(app);
    gboolean success = TRUE;
    for (guint i = 0; i < all->len && !app->stopping; i++) {
        Device *child = all->pdata[i];
        if (g_strcmp0(child->table, device->path) || !device_handled(app, child))
            continue;
        g_autoptr(GError) child_error = NULL;
        if (!device_add(app, child, flags, &child_error)) {
            if (success)
                g_propagate_error(error, g_steal_pointer(&child_error));
            success = FALSE;
        }
    }
    return success;
}

gboolean device_add(App *app, Device *device, Flags flags, GError **error)
{
    if (flags & AUTOMATIC) {
        if (!device_auto(app, device) || !device->media)
            return TRUE;
    } else if (!device_handled(app, device)) {
        return fail(error, G_IO_ERROR_PERMISSION_DENIED, "Device is ignored by policy");
    }
    if (device->filesystem)
        return mount_fs(app, device, error);
    if (device->encrypted) {
        if ((flags & AUTOMATIC) && app->config.prompt_disabled && !device->cleartext &&
            !device_rule(app, device, "keyfile"))
            return TRUE;
        g_autofree char *cleartext = unlock(app, device, error);
        if (!cleartext)
            return FALSE;
        if (!device->cleartext)
            operation_log(app, "unlocked", device->presentation, NULL);
        if (!(flags & RECURSIVE))
            return TRUE;
        g_autoptr(Device) child = wait_device(app, cleartext, error);
        return child && device_add(app, child, flags, error);
    }
    if (device->partition_table && (flags & RECURSIVE))
        return add_children(app, device, flags, error);
    if (flags & AUTOMATIC)
        return TRUE;
    return fail(error, G_IO_ERROR_NOT_SUPPORTED, "No mountable filesystem or encrypted volume");
}

static gboolean unmount_tree(App *app, Device *device, Flags flags, GError **error)
{
    if (flags & FORCE) {
        g_autoptr(GPtrArray) all = devices_get(app);
        for (guint i = 0; i < all->len; i++) {
            Device *child = all->pdata[i];
            if (!g_strcmp0(device_parent(child), device->path) && device_handled(app, child) &&
                !unmount_tree(app, child, flags, error))
                return FALSE;
        }
    }
    if (device->filesystem && device->mounts[0]) {
        if (!simple_call(app, device->path, IFACE("Filesystem"), "Unmount", NULL, error))
            return FALSE;
        operation_log(app, "unmounted", device->presentation, NULL);
    }
    if (device->encrypted && device->cleartext) {
        if (!simple_call(app, device->path, IFACE("Encrypted"), "Lock", NULL, error))
            return FALSE;
        operation_log(app, "locked", device->presentation, NULL);
    }
    return TRUE;
}

static Device *root_device(App *app, Device *device)
{
    Device *root = device_get(app, device->path);
    for (unsigned depth = 0; root && device_parent(root) && depth < 64; depth++) {
        Device *parent = device_get(app, device_parent(root));
        if (!parent)
            break;
        device_free(root);
        root = parent;
    }
    return root;
}

gboolean device_delete(App *app, Device *device, GError **error)
{
    g_autoptr(Device) current = device_get(app, device->path);
    if (!current && device->autoclear)
        return TRUE;
    if (!simple_call(app, device->path, IFACE("Loop"), "Delete", NULL, error))
        return FALSE;
    operation_log(app, "deleted", device->presentation, NULL);
    return TRUE;
}

gboolean device_remove(App *app, Device *device, Flags flags, GError **error)
{
    if (!device_handled(app, device))
        return fail(error, G_IO_ERROR_PERMISSION_DENIED, "Device is ignored by policy");
    g_autoptr(Device) root = root_device(app, device);
    if (!root)
        return fail(error, G_IO_ERROR_NOT_FOUND, "Device disappeared");
    gboolean delete_loop = root->loop && ((flags & DETACH) ||
        (!(flags & KEEP_LOOP) && !strcmp(root->path, device->path)));
    const char *drive = root->drive;
    gboolean eject = (flags & EJECT) && drive && root->ejectable && root->media;
    gboolean detach = (flags & DETACH) && drive && root->detachable;
    if (!(flags & AUTOMATIC) && ((flags & EJECT && !eject) ||
        (flags & DETACH && !detach && !delete_loop)))
        return fail(error, G_IO_ERROR_NOT_SUPPORTED, "Drive does not support the requested removal");

    if (!device->filesystem && !device->encrypted && !(flags & FORCE) &&
        !delete_loop && !eject && !detach)
        return fail(error, G_IO_ERROR_NOT_SUPPORTED, "Use --force to unmount child devices");
    if (!unmount_tree(app, device, flags, error))
        return FALSE;

    if ((flags & LOCK) && device->backing) {
        g_autoptr(Device) backing = device_get(app, device->backing);
        if (backing && device_handled(app, backing) && !unmount_tree(app, backing, flags, error))
            return FALSE;
    }
    if (delete_loop) {
        if ((flags & FORCE) && strcmp(root->path, device->path) &&
            !unmount_tree(app, root, flags, error))
            return FALSE;
        return device_delete(app, root, error);
    }
    if ((eject || detach) && (flags & FORCE)) {
        g_autoptr(GPtrArray) all = devices_get(app);
        for (guint i = 0; i < all->len; i++) {
            Device *peer = all->pdata[i];
            if (!device_parent(peer) && !g_strcmp0(peer->drive, drive) && device_handled(app, peer) &&
                !unmount_tree(app, peer, flags, error))
                return FALSE;
        }
    }
    if (eject && !simple_call(app, drive, IFACE("Drive"), "Eject", NULL, error))
        return FALSE;
    if (detach && !simple_call(app, drive, IFACE("Drive"), "PowerOff", NULL, error))
        return FALSE;
    if (eject || detach)
        operation_log(app, detach ? "detached" : "ejected", device->presentation, NULL);
    return TRUE;
}

Device *loop_setup(App *app, const char *file, GError **error)
{
    gboolean read_only = FALSE;
    int fd = g_open(file, O_RDWR | O_CLOEXEC, 0);
    if (fd < 0 && (errno == EACCES || errno == EPERM || errno == EROFS)) {
        read_only = TRUE;
        fd = g_open(file, O_RDONLY | O_CLOEXEC, 0);
    }
    if (fd < 0) {
        int code = errno;
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(code), "%s", g_strerror(code));
        return NULL;
    }
    g_autoptr(GUnixFDList) fds = g_unix_fd_list_new();
    int handle = g_unix_fd_list_append(fds, fd, error);
    close(fd);
    if (handle < 0)
        return NULL;
    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&options, "{sv}", "read-only", g_variant_new_boolean(read_only));
    g_autoptr(GVariant) reply = call_with_fds(app, ROOT_PATH "/Manager", IFACE("Manager"),
        "LoopSetup", g_variant_new("(ha{sv})", handle, &options), G_VARIANT_TYPE("(o)"), fds, error);
    if (!reply)
        return NULL;
    const char *path;
    g_variant_get(reply, "(&o)", &path);
    Device *device = wait_device(app, path, error);
    if (device)
        operation_log(app, "set up", file, device->presentation);
    return device;
}
