#include "cdiskie.h"

#include <math.h>
#include <string.h>

void launch_command(char **argv)
{
    if (!argv || !argv[0])
        return;
    g_autoptr(GError) error = NULL;
    if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &error))
        g_printerr("cdiskie: %s: %s\n", argv[0], error->message);
}

#ifdef WITH_DESKTOP

#include <gtk/gtk.h>
#include <libnotify/notify.h>
#ifdef WITH_INDICATOR
#include <libayatana-appindicator/app-indicator.h>
#endif

/* GtkStatusIcon is the legacy X11 tray backend. */
G_GNUC_BEGIN_IGNORE_DEPRECATIONS

typedef enum { ADD, UNMOUNT, UNLOCK, LOCK_DEVICE, BROWSE, OPEN_TERMINAL, POWER_OFF, EJECT_MEDIA, DELETE } Action;

typedef struct {
    App *app;
    char *path;
    Action action;
} MenuAction;

typedef struct {
    GtkStatusIcon *status;
    GtkWidget *menu, *quick;
#ifdef WITH_INDICATOR
    AppIndicator *indicator;
#endif
    GHashTable *notifications;
    gboolean notify;
} Desktop;

static void action_free(gpointer data)
{
    MenuAction *action = data;
    g_free(action->path);
    g_free(action);
}

static MenuAction *action_new(App *app, Device *device, Action action)
{
    MenuAction *entry = g_new(MenuAction, 1);
    *entry = (MenuAction){ app, g_strdup(device->path), action };
    return entry;
}

static const char *action_name(Action action)
{
    static const char *names[] = {
        "mount", "unmount", "unlock", "lock", "browse", "terminal", "detach", "eject", "delete"
    };
    return names[action];
}

static const char *action_label(Action action)
{
    static const char *labels[] = {
        "Mount", "Unmount", "Unlock", "Lock", "Open in File Manager",
        "Open Terminal", "Power Off", "Eject", "Detach Disk Image"
    };
    return labels[action];
}

static char *configured_icon(App *app, const char *key, const char *fallback)
{
    yaml_node_t *icons = config_section(&app->config, "icon_names");
    g_auto(GStrv) names = config_list(&app->config, config_get(&app->config, icons, key));
    GtkIconTheme *theme = gtk_icon_theme_get_default();
    for (char **name = names; theme && *name; name++)
        if (gtk_icon_theme_has_icon(theme, *name))
            return g_strdup(*name);
    return g_strdup(fallback);
}

static void perform_action(MenuAction *action)
{
    App *app = action->app;
    if (app->busy || app->stopping)
        return;
    g_autoptr(Device) device = device_get(app, action->path);
    if (!device || !device_handled(app, device))
        return;
    app->busy = TRUE;
    g_autoptr(GError) error = NULL;
    switch (action->action) {
    case ADD:
    case UNLOCK:
        device_add(app, device, RECURSIVE, &error);
        break;
    case UNMOUNT:
        device_remove(app, device, KEEP_LOOP, &error);
        break;
    case LOCK_DEVICE:
        device_remove(app, device, FORCE | KEEP_LOOP, &error);
        break;
    case POWER_OFF:
    case DELETE:
        device_remove(app, device, FORCE | DETACH | LOCK, &error);
        break;
    case EJECT_MEDIA:
        device_remove(app, device, FORCE | EJECT | LOCK, &error);
        break;
    case BROWSE:
    case OPEN_TERMINAL:
        if (device->mounts[0]) {
            char **command = action->action == BROWSE ? app->config.browser : app->config.terminal;
            g_auto(GStrv) argv = command_format(command, device, NULL, device->mounts[0]);
            launch_command(argv);
        }
        break;
    }
    if (error)
        operation_failed(app, device, error);
    app->busy = FALSE;
}

static void menu_action(GtkMenuItem *item, gpointer data)
{
    (void)item;
    perform_action(data);
}

static void notification_action(NotifyNotification *notification, char *name, gpointer data)
{
    (void)notification;
    (void)name;
    perform_action(data);
}

static void notification_closed(NotifyNotification *notification, gpointer data)
{
    Desktop *desktop = data;
    g_hash_table_remove(desktop->notifications, notification);
}

static void append_item(GtkWidget *menu, GtkWidget *item)
{
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    gtk_widget_show(item);
}

static void action_item(App *app, GtkWidget *menu, Device *device, Action action, const char *label)
{
    static const char *fallback[] = {
        "media-playback-start", "media-eject", "changes-allow", "changes-prevent",
        "folder-open", "utilities-terminal", "system-shutdown", "media-eject", "media-eject"
    };
    g_autofree char *icon = configured_icon(app, action_name(action), fallback[action]);
    GtkWidget *item = gtk_image_menu_item_new_with_label(label);
    gtk_image_menu_item_set_image(GTK_IMAGE_MENU_ITEM(item), gtk_image_new_from_icon_name(icon,
                                                                                         GTK_ICON_SIZE_MENU));
    MenuAction *entry = action_new(app, device, action);
    g_object_set_data_full(G_OBJECT(item), "cdiskie-action", entry, action_free);
    g_signal_connect(item, "activate", G_CALLBACK(menu_action), entry);
    append_item(menu, item);
}

static gboolean quick_action(App *app, Action action)
{
    yaml_node_t *node = config_section(&app->config, "quickmenu_actions");
    if (!node)
        return action == ADD || action == BROWSE || action == UNLOCK || action == OPEN_TERMINAL ||
               action == DELETE || action == POWER_OFF;
    g_auto(GStrv) names = config_list(&app->config, node);
    return g_strv_contains((const char *const *)names, "all") ||
           g_strv_contains((const char *const *)names, action_name(action));
}

static const char *device_label(Device *device)
{
    return device->label ? device->label : device->presentation;
}

static void open_image(GtkMenuItem *item, gpointer data)
{
    (void)item;
    App *app = data;
    if (app->busy || app->stopping)
        return;
    app->busy = TRUE;
    GtkWidget *dialog = gtk_file_chooser_dialog_new("Open Disk Image", NULL,
        GTK_FILE_CHOOSER_ACTION_OPEN, "_Cancel", GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, NULL);
    app->dialog = dialog;
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        g_autofree char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        gtk_widget_hide(dialog);
        if (path) {
            g_autoptr(GError) error = NULL;
            g_autoptr(Device) device = loop_setup(app, path, &error);
            if (device) {
                if (!device_add(app, device, RECURSIVE, &error))
                    operation_failed(app, device, error);
            } else if (error) {
                g_printerr("cdiskie: %s: %s\n", path, error->message);
            }
        }
    }
    app->dialog = NULL;
    gtk_widget_destroy(dialog);
    app->busy = FALSE;
}

static void quit_menu(GtkMenuItem *item, gpointer data)
{
    (void)item;
    App *app = data;
    app->stopping = TRUE;
    g_cancellable_cancel(app->cancel);
    desktop_cancel(app);
    g_main_loop_quit(app->loop);
}

static void toggle_setting(GtkMenuItem *item, gpointer data)
{
    gboolean *enabled = data;
    if (GTK_IS_CHECK_MENU_ITEM(item)) {
        *enabled = gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(item));
    } else {
        *enabled = !*enabled;
        const char *setting = g_object_get_data(G_OBJECT(item), "cdiskie-setting");
        g_autofree char *label = g_strdup_printf("%s %s", *enabled ? "Disable" : "Enable", setting);
        gtk_menu_item_set_label(item, label);
    }
}

static void setting_item(App *app, GtkWidget *menu, const char *label, gboolean *enabled)
{
    GtkWidget *item;
    if (app->config.checkbox_workaround) {
        g_autofree char *text = g_strdup_printf("%s %s", *enabled ? "Disable" : "Enable", label);
        item = gtk_menu_item_new_with_label(text);
        g_object_set_data(G_OBJECT(item), "cdiskie-setting", (gpointer)label);
    } else {
        item = gtk_check_menu_item_new_with_label(label);
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), *enabled);
    }
    g_signal_connect(item, "activate", G_CALLBACK(toggle_setting), enabled);
    append_item(menu, item);
}

static void forget_passwords(GtkMenuItem *item, gpointer data)
{
    (void)item;
    App *app = data;
    g_hash_table_remove_all(app->passwords);
}

static void clear_menu(GtkWidget *menu)
{
    GList *children = gtk_container_get_children(GTK_CONTAINER(menu));
    for (GList *child = children; child; child = child->next)
        gtk_widget_destroy(child->data);
    g_list_free(children);
}

static void reset_menu(GtkWidget **menu, gboolean reuse)
{
    if (*menu && reuse) {
        clear_menu(*menu);
        return;
    }
    if (*menu) {
        gtk_widget_destroy(*menu);
        g_object_unref(*menu);
    }
    *menu = g_object_ref_sink(gtk_menu_new());
}

static void populate_actions(App *app, GPtrArray *devices, guint index, Action *actions, guint *count)
{
    Device *device = devices->pdata[index];
    if (device->filesystem) {
        if (device->mounts[0]) {
            if (app->config.browser && app->config.browser[0])
                actions[(*count)++] = BROWSE;
            if (app->config.terminal && app->config.terminal[0])
                actions[(*count)++] = OPEN_TERMINAL;
            actions[(*count)++] = UNMOUNT;
        } else if (device->media) {
            actions[(*count)++] = ADD;
        }
    } else if (device->encrypted) {
        if (device->cleartext)
            actions[(*count)++] = LOCK_DEVICE;
        else if (!app->config.prompt_disabled || device_rule(app, device, "keyfile"))
            actions[(*count)++] = UNLOCK;
    }
    if (device->loop)
        actions[(*count)++] = DELETE;
    if (device_parent(device) || !device->drive)
        return;
    for (guint i = 0; i < index; i++) {
        Device *other = devices->pdata[i];
        if (!device_parent(other) && !g_strcmp0(other->drive, device->drive) &&
            device_handled(app, other))
            return;
    }
    if (device->ejectable && device->media)
        actions[(*count)++] = EJECT_MEDIA;
    if (device->detachable)
        actions[(*count)++] = POWER_OFF;
}

static GtkWidget *device_submenu(App *app, GHashTable *menus, GtkWidget *menu, Device *device,
                                unsigned depth)
{
    GtkWidget *submenu = g_hash_table_lookup(menus, device->path);
    if (submenu)
        return submenu;
    if (depth < 64) {
        g_autoptr(Device) parent = device_get(app, device_parent(device));
        if (parent && device_handled(app, parent))
            menu = device_submenu(app, menus, menu, parent, depth + 1);
    }
    GtkWidget *item = gtk_menu_item_new_with_label(device_label(device));
    submenu = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), submenu);
    append_item(menu, item);
    g_hash_table_insert(menus, g_strdup(device->path), submenu);
    return submenu;
}

void desktop_refresh(App *app)
{
    Desktop *desktop = app->desktop;
    if (!desktop || (!desktop->status
#ifdef WITH_INDICATOR
                     && !desktop->indicator
#endif
                     ))
        return;
    reset_menu(&desktop->menu, app->config.menu_workaround);
    reset_menu(&desktop->quick, FALSE);
    g_autoptr(GPtrArray) devices = devices_get(app);
    g_autoptr(GHashTable) submenus = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GtkWidget *device_menu = desktop->menu;
    if (app->config.menu_workaround) {
        GtkWidget *managed = gtk_menu_item_new_with_label("Managed Devices");
        device_menu = gtk_menu_new();
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(managed), device_menu);
        append_item(desktop->menu, managed);
    }
    guint actions_count = 0;
    for (guint i = 0; i < devices->len; i++) {
        Device *device = devices->pdata[i];
        if (!device_handled(app, device))
            continue;
        Action actions[6];
        guint count = 0;
        populate_actions(app, devices, i, actions, &count);
        if (!count)
            continue;
        actions_count += count;
        GtkWidget *submenu = device_menu;
        if (!app->config.flat)
            submenu = device_submenu(app, submenus, submenu, device, 0);
        for (guint j = 0; j < count; j++) {
            Action action = actions[j];
            g_autofree char *label = g_strdup_printf("%s %s", action_label(action), device_label(device));
            action_item(app, submenu, device, action, app->config.flat ? label : action_label(action));
            if (quick_action(app, action))
                action_item(app, desktop->quick, device, action, label);
        }
    }
    if (actions_count)
        append_item(desktop->menu, gtk_separator_menu_item_new());
    GtkWidget *image = gtk_menu_item_new_with_label("Open Disk Image…");
    g_signal_connect(image, "activate", G_CALLBACK(open_image), app);
    append_item(desktop->menu, image);
    setting_item(app, desktop->menu, "Automount", &app->config.automount);
    setting_item(app, desktop->menu, "Notifications", &app->config.notify);
    if (app->config.cache_minutes > 0) {
        GtkWidget *clear = gtk_menu_item_new_with_label("Clear Passphrase Cache");
        g_signal_connect(clear, "activate", G_CALLBACK(forget_passwords), app);
        append_item(desktop->menu, clear);
    }
    GtkWidget *quit = gtk_menu_item_new_with_label("Quit");
    g_signal_connect(quit, "activate", G_CALLBACK(quit_menu), app);
    append_item(desktop->menu, quit);

    gboolean visible = app->config.tray != 2 || actions_count;
    if (desktop->status)
        gtk_status_icon_set_visible(desktop->status, visible);
#ifdef WITH_INDICATOR
    if (desktop->indicator) {
        app_indicator_set_menu(desktop->indicator, GTK_MENU(desktop->menu));
        app_indicator_set_status(desktop->indicator, visible ?
                                 APP_INDICATOR_STATUS_ACTIVE : APP_INDICATOR_STATUS_PASSIVE);
    }
#endif
}

static void popup(GtkStatusIcon *icon, guint button, guint time, gpointer data)
{
    Desktop *desktop = data;
    if (desktop->menu)
        gtk_menu_popup(GTK_MENU(desktop->menu), NULL, NULL,
                       gtk_status_icon_position_menu, icon, button, time);
}

static void activate(GtkStatusIcon *icon, gpointer data)
{
    App *app = data;
    Desktop *desktop = app->desktop;
    if (app->config.flat && desktop->quick) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(desktop->quick));
        gboolean have_actions = children != NULL;
        g_list_free(children);
        if (have_actions) {
            gtk_menu_popup(GTK_MENU(desktop->quick), NULL, NULL, gtk_status_icon_position_menu,
                           icon, 0, gtk_get_current_event_time());
            return;
        }
    }
    popup(icon, 0, gtk_get_current_event_time(), desktop);
}

void desktop_init(App *app)
{
    Desktop *desktop = g_new0(Desktop, 1);
    app->desktop = desktop;
    if (!app->config.tray && !app->config.indicator)
        return;
    if (!gtk_init_check(NULL, NULL)) {
        g_printerr("cdiskie: a graphical session is required for the tray icon\n");
        return;
    }
    if (!app->config.indicator_set && g_getenv("WAYLAND_DISPLAY"))
        app->config.indicator = TRUE;
    g_autofree char *icon = configured_icon(app, "media", "drive-removable-media");
#ifdef WITH_INDICATOR
    if (app->config.indicator) {
        desktop->indicator = app_indicator_new("cdiskie", icon, APP_INDICATOR_CATEGORY_HARDWARE);
        app_indicator_set_title(desktop->indicator, "Removable Media");
        return;
    }
#else
    if (app->config.indicator)
        g_printerr("cdiskie: AppIndicator support is unavailable in this build\n");
#endif
    desktop->status = gtk_status_icon_new_from_icon_name(icon);
    gtk_status_icon_set_visible(desktop->status, FALSE);
    gtk_status_icon_set_title(desktop->status, "Removable Media");
    gtk_status_icon_set_tooltip_text(desktop->status, "Removable Media");
    g_signal_connect(desktop->status, "activate", G_CALLBACK(activate), app);
    g_signal_connect(desktop->status, "popup-menu", G_CALLBACK(popup), desktop);
}

static void destroy_menu(GtkWidget *menu)
{
    if (menu) {
        gtk_widget_destroy(menu);
        g_object_unref(menu);
    }
}

void desktop_clear(App *app)
{
    Desktop *desktop = app->desktop;
    if (!desktop)
        return;
#ifdef WITH_INDICATOR
    g_clear_object(&desktop->indicator);
#endif
    g_clear_object(&desktop->status);
    destroy_menu(desktop->menu);
    destroy_menu(desktop->quick);
    if (desktop->notifications) {
        GHashTableIter iter;
        gpointer notification;
        g_hash_table_iter_init(&iter, desktop->notifications);
        while (g_hash_table_iter_next(&iter, &notification, NULL)) {
            g_signal_handlers_disconnect_by_data(notification, desktop);
            notify_notification_close(notification, NULL);
        }
        g_hash_table_unref(desktop->notifications);
    }
    if (desktop->notify)
        notify_uninit();
    g_clear_pointer(&app->desktop, g_free);
}

void desktop_cancel(App *app)
{
    if (app->dialog)
        gtk_dialog_response(GTK_DIALOG(app->dialog), GTK_RESPONSE_CANCEL);
}

static void show_passphrase(GtkToggleButton *button, gpointer data)
{
    gtk_entry_set_visibility(GTK_ENTRY(data), gtk_toggle_button_get_active(button));
}

gboolean desktop_password(App *app, Device *device, Secret *secret, char **keyfile)
{
    if (!gtk_init_check(NULL, NULL))
        return FALSE;
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Unlock Encrypted Volume", NULL,
        GTK_DIALOG_MODAL, "Choose _Keyfile…", GTK_RESPONSE_APPLY,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Unlock", GTK_RESPONSE_ACCEPT, NULL);
    gpointer previous = app->dialog;
    app->dialog = dialog;
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(box), 16);
    gtk_box_set_spacing(GTK_BOX(box), 10);
    g_autofree char *label = g_strdup_printf("Passphrase for %s", device_label(device));
    GtkWidget *description = gtk_label_new(label);
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(entry), FALSE);
    gtk_entry_set_input_purpose(GTK_ENTRY(entry), GTK_INPUT_PURPOSE_PASSWORD);
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_box_pack_start(GTK_BOX(box), description, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), entry, FALSE, FALSE, 0);
    GtkWidget *show = gtk_check_button_new_with_label("Show passphrase");
    g_signal_connect(show, "toggled", G_CALLBACK(show_passphrase), entry);
    gtk_box_pack_start(GTK_BOX(box), show, FALSE, FALSE, 0);
    GtkWidget *remember = gtk_check_button_new_with_label("Cache passphrase");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(remember), secret->remember);
    if (!secret->remember)
        gtk_widget_set_no_show_all(remember, TRUE);
    gtk_box_pack_start(GTK_BOX(box), remember, FALSE, FALSE, 0);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    gtk_widget_show_all(box);
    gtk_widget_grab_focus(entry);
    int response;
    do {
        response = gtk_dialog_run(GTK_DIALOG(dialog));
        if (response == GTK_RESPONSE_APPLY) {
            GtkWidget *picker = gtk_file_chooser_dialog_new("Choose Keyfile", GTK_WINDOW(dialog),
                GTK_FILE_CHOOSER_ACTION_OPEN, "_Cancel", GTK_RESPONSE_CANCEL,
                "_Open", GTK_RESPONSE_ACCEPT, NULL);
            app->dialog = picker;
            if (gtk_dialog_run(GTK_DIALOG(picker)) == GTK_RESPONSE_ACCEPT)
                *keyfile = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(picker));
            app->dialog = dialog;
            gtk_widget_destroy(picker);
        }
    } while (response == GTK_RESPONSE_APPLY && !*keyfile && !app->stopping);
    if (response == GTK_RESPONSE_ACCEPT) {
        secret->data = g_strdup(gtk_entry_get_text(GTK_ENTRY(entry)));
        secret->length = strlen(secret->data);
    }
    secret->remember = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(remember));
    gtk_entry_set_text(GTK_ENTRY(entry), "");
    gtk_widget_destroy(dialog);
    app->dialog = previous;
    return secret->data || *keyfile;
}

static int notification_timeout(App *app, const char *event, gboolean enabled)
{
    yaml_node_t *section = config_section(&app->config, "notifications");
    yaml_node_t *node = config_get(&app->config, section, event);
    if (!config_bool(node, TRUE) || (!node && !enabled))
        return 0;
    if (!node)
        node = config_get(&app->config, section, "timeout");
    const char *value = config_scalar(node);
    if (!value)
        return NOTIFY_EXPIRES_DEFAULT;
    char *end;
    double seconds = g_ascii_strtod(value, &end);
    if (end == value || *end || !isfinite(seconds) || seconds < 0)
        return NOTIFY_EXPIRES_DEFAULT;
    return (int)MIN(seconds * 1000, G_MAXINT);
}

static void notify_event(App *app, Device *device, const char *event, const char *detail)
{
    Desktop *desktop = app->desktop;
    if (!desktop || !app->config.notify)
        return;
    static const struct { const char *event, *title; gboolean enabled; } events[] = {
        { "device_added", "Device Connected", TRUE },
        { "device_removed", "Device Disconnected", TRUE },
        { "device_mounted", "Device Mounted", TRUE },
        { "device_unmounted", "Device Unmounted", TRUE },
        { "device_unlocked", "Volume Unlocked", TRUE },
        { "device_locked", "Volume Locked", TRUE },
        { "media_added", "Media Inserted", FALSE },
        { "media_removed", "Media Removed", FALSE },
        { "job_failed", "Device Operation Failed", TRUE },
    };
    const char *title = NULL;
    int timeout = 0;
    for (gsize i = 0; i < G_N_ELEMENTS(events); i++) {
        if (!strcmp(event, events[i].event)) {
            title = events[i].title;
            timeout = notification_timeout(app, event, events[i].enabled);
            break;
        }
    }
    if (!title || !timeout)
        return;
    if ((!strcmp(event, "device_added") || !strcmp(event, "device_removed")) && device_parent(device))
        return;
    if (!desktop->notify) {
        desktop->notify = notify_init("cdiskie");
        if (!desktop->notify)
            return;
        desktop->notifications = g_hash_table_new_full(g_direct_hash, g_direct_equal, g_object_unref, NULL);
    }
    g_autofree char *body = g_markup_printf_escaped("%s%s%s", device_label(device),
                                                     detail ? "\n" : "", detail ? detail : "");
    NotifyNotification *notification = notify_notification_new(title, body, "drive-removable-media");
    notify_notification_set_timeout(notification, timeout);
    yaml_node_t *section = config_section(&app->config, "notification_actions");
    yaml_node_t *node = config_get(&app->config, section, event);
    g_auto(GStrv) actions = config_list(&app->config, node);
    if (!strcmp(event, "device_mounted") && app->config.browser && app->config.browser[0] &&
        (!node || g_strv_contains((const char *const *)actions, "browse")))
        notify_notification_add_action(notification, "browse", action_label(BROWSE),
            notification_action, action_new(app, device, BROWSE), action_free);
    if (!strcmp(event, "device_mounted") && app->config.terminal && app->config.terminal[0] &&
        (!node || g_strv_contains((const char *const *)actions, "terminal")))
        notify_notification_add_action(notification, "terminal", action_label(OPEN_TERMINAL),
            notification_action, action_new(app, device, OPEN_TERMINAL), action_free);
    if (!strcmp(event, "device_added") &&
        (device->filesystem || device->encrypted || device->partition_table) &&
        (node ? g_strv_contains((const char *const *)actions, "mount") : !app->config.automount))
        notify_notification_add_action(notification, "mount", action_label(ADD),
            notification_action, action_new(app, device, ADD), action_free);
    g_signal_connect(notification, "closed", G_CALLBACK(notification_closed), desktop);
    if (notify_notification_show(notification, NULL))
        g_hash_table_add(desktop->notifications, notification);
    else
        g_object_unref(notification);
}

G_GNUC_END_IGNORE_DEPRECATIONS

#else

void desktop_init(App *app)
{
    if (app->config.tray || app->config.indicator)
        g_printerr("cdiskie: tray support is unavailable in this build\n");
}

void desktop_clear(App *app) { (void)app; }
void desktop_cancel(App *app) { (void)app; }
void desktop_refresh(App *app) { (void)app; }

gboolean desktop_password(App *app, Device *device, Secret *secret, char **keyfile)
{
    (void)app;
    (void)device;
    (void)secret;
    (void)keyfile;
    return FALSE;
}

#endif

void desktop_event(App *app, Device *device, const char *event, const char *detail)
{
    if (app->mode != DAEMON || app->stopping)
        return;
    g_auto(GStrv) hook = command_format(app->config.hook, device, event, NULL);
    launch_command(hook);
#ifdef WITH_DESKTOP
    notify_event(app, device, event, detail);
#else
    (void)detail;
#endif
}
