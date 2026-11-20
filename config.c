#include "cdiskie.h"

#include <errno.h>
#include <fnmatch.h>
#include <glib/gstdio.h>
#include <string.h>

const char *config_scalar(yaml_node_t *node)
{
    return node && node->type == YAML_SCALAR_NODE ? (char *)node->data.scalar.value : NULL;
}

yaml_node_t *config_get(Config *config, yaml_node_t *map, const char *key)
{
    if (!map || map->type != YAML_MAPPING_NODE)
        return NULL;

    for (yaml_node_pair_t *pair = map->data.mapping.pairs.start;
         pair < map->data.mapping.pairs.top; pair++) {
        const char *name = config_scalar(yaml_document_get_node(&config->document, pair->key));
        if (g_strcmp0(name, key) == 0)
            return yaml_document_get_node(&config->document, pair->value);
    }
    return NULL;
}

yaml_node_t *config_section(Config *config, const char *name)
{
    if (!config->loaded)
        return NULL;
    return config_get(config, yaml_document_get_root_node(&config->document), name);
}

gboolean config_bool(yaml_node_t *node, gboolean fallback)
{
    const char *value = config_scalar(node);
    if (!value)
        return fallback;
    if (!g_ascii_strcasecmp(value, "true") || !g_ascii_strcasecmp(value, "yes") ||
        !g_ascii_strcasecmp(value, "on") || !strcmp(value, "1"))
        return TRUE;
    if (!g_ascii_strcasecmp(value, "false") || !g_ascii_strcasecmp(value, "no") ||
        !g_ascii_strcasecmp(value, "off") || !strcmp(value, "0"))
        return FALSE;
    return fallback;
}

char **config_list(Config *config, yaml_node_t *node)
{
    GPtrArray *list = g_ptr_array_new_with_free_func(g_free);
    if (node && node->type == YAML_SEQUENCE_NODE) {
        for (yaml_node_item_t *item = node->data.sequence.items.start;
             item < node->data.sequence.items.top; item++) {
            const char *value = config_scalar(yaml_document_get_node(&config->document, *item));
            if (value)
                g_ptr_array_add(list, g_strdup(value));
        }
    } else if (config_scalar(node)) {
        g_ptr_array_add(list, g_strdup(config_scalar(node)));
    }
    g_ptr_array_add(list, NULL);
    return (char **)g_ptr_array_free(list, FALSE);
}

char **command_parse(const char *command, GError **error)
{
    char **argv = NULL;
    if (command && *command)
        g_shell_parse_argv(command, NULL, &argv, error);
    return argv;
}

static gboolean read_command(Config *config, yaml_node_t *options, const char *key,
                             char ***destination, GError **error)
{
    yaml_node_t *node = config_get(config, options, key);
    if (!node)
        return TRUE;

    g_clear_pointer(destination, g_strfreev);
    if (node->type == YAML_SEQUENCE_NODE)
        *destination = config_list(config, node);
    else if (config_bool(node, TRUE)) {
        const char *value = config_scalar(node);
        if (value && strcmp(value, "null") && strcmp(value, "~"))
            *destination = command_parse(value, error);
    }
    return !error || !*error;
}

static const char *default_config(char **filename)
{
    const char *extensions[] = { "yml", "yaml", "json" };
    const char *directories[] = { "cdiskie", "udiskie" };
    for (gsize dir = 0; dir < G_N_ELEMENTS(directories); dir++) {
        for (gsize ext = 0; ext < G_N_ELEMENTS(extensions); ext++) {
            g_autofree char *name = g_strdup_printf("config.%s", extensions[ext]);
            *filename = g_build_filename(g_get_user_config_dir(), directories[dir], name, NULL);
            if (g_file_test(*filename, G_FILE_TEST_EXISTS))
                return *filename;
            g_clear_pointer(filename, g_free);
        }
    }
    return NULL;
}

gboolean config_load(Config *config, const char *path, gboolean disabled, GError **error)
{
    config->automount = TRUE;
    config->notify = TRUE;
    config->flat = TRUE;
    config->browser = command_parse("xdg-open", NULL);
    config->menu_workaround = g_getenv("WAYLAND_DISPLAY") != NULL;

    if (disabled)
        return TRUE;
    g_autofree char *filename = NULL;
    if (!path)
        path = default_config(&filename);
    if (!path)
        return TRUE;

    FILE *file = g_fopen(path, "rb");
    if (!file) {
        int code = errno;
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(code),
                    "%s: %s", path, g_strerror(code));
        return FALSE;
    }
    yaml_parser_t parser;
    if (!yaml_parser_initialize(&parser)) {
        fclose(file);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Cannot initialize YAML parser");
        return FALSE;
    }
    yaml_parser_set_input_file(&parser, file);
    config->loaded = yaml_parser_load(&parser, &config->document);
    if (!config->loaded)
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "%s:%zu:%zu: %s", path,
                    parser.problem_mark.line + 1, parser.problem_mark.column + 1, parser.problem);
    yaml_parser_delete(&parser);
    fclose(file);
    if (!config->loaded)
        return FALSE;

    yaml_node_t *root = yaml_document_get_root_node(&config->document);
    if (root && root->type != YAML_MAPPING_NODE) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "%s: expected a mapping", path);
        return FALSE;
    }

    yaml_node_t *options = config_section(config, "program_options");
    config->automount = config_bool(config_get(config, options, "automount"), config->automount);
    config->notify = config_bool(config_get(config, options, "notify"), config->notify);
    yaml_node_t *indicator = config_get(config, options, "appindicator");
    config->indicator = config_bool(indicator, FALSE);
    config->indicator_set = indicator != NULL;
    config->checkbox_workaround = config_bool(config_get(config, options, "menu_checkbox_workaround"), FALSE);
    config->menu_workaround = config_bool(config_get(config, options, "menu_update_workaround"),
                                          g_getenv("WAYLAND_DISPLAY") != NULL);
    yaml_node_t *tray = config_get(config, options, "tray");
    config->tray = g_strcmp0(config_scalar(tray), "auto") == 0 ? 2 : config_bool(tray, FALSE);
    const char *menu = config_scalar(config_get(config, options, "menu"));
    config->flat = g_strcmp0(menu, "nested") != 0 && g_strcmp0(menu, "smart") != 0;
    const char *minutes = config_scalar(config_get(config, options, "password_cache"));
    if (minutes)
        config->cache_minutes = g_ascii_strtod(minutes, NULL);
    config->prompt_disabled = !config_bool(config_get(config, options, "password_prompt"), TRUE);

    gboolean ok = read_command(config, options, "file_manager", &config->browser, error) &&
           read_command(config, options, "terminal", &config->terminal, error) &&
           read_command(config, options, "password_prompt", &config->prompt, error) &&
           read_command(config, options, "notify_command", &config->hook, error) &&
           read_command(config, options, "event_hook", &config->hook, error);
    if (config_get(config, options, "password_prompt") && (!config->prompt || !config->prompt[0]))
        config->prompt_disabled = TRUE;
    return ok;
}

void config_clear(Config *config)
{
    if (config->loaded)
        yaml_document_delete(&config->document);
    g_strfreev(config->prompt);
    g_strfreev(config->hook);
    g_strfreev(config->browser);
    g_strfreev(config->terminal);
    g_free(config->mount_options);
}

static char *format_word(const char *word, Device *device, const char *event)
{
    GString *result = g_string_new(NULL);
    for (const char *at = word; *at; at++) {
        if ((*at == '{' || *at == '}') && at[1] == *at) {
            g_string_append_c(result, *at++);
            continue;
        }
        const char *end = *at == '{' ? strchr(at + 1, '}') : NULL;
        if (!end) {
            g_string_append_c(result, *at);
            continue;
        }
        g_autofree char *key = g_strndup(at + 1, end - at - 1);
        g_auto(GStrv) values = device ? device_values(device, key) : NULL;
        if (!strcmp(key, "event"))
            g_string_append(result, event ? event : "");
        else if (values)
            g_string_append(result, values[0] ? values[0] : "");
        else
            g_string_append_len(result, at, end - at + 1);
        at = end;
    }
    return g_string_free(result, FALSE);
}

char **command_format(char **command, Device *device, const char *event, const char *extra)
{
    if (!command || !command[0])
        return NULL;
    GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);
    for (char **word = command; *word; word++)
        g_ptr_array_add(argv, format_word(*word, device, event));
    if (extra)
        g_ptr_array_add(argv, g_strdup(extra));
    g_ptr_array_add(argv, NULL);
    return (char **)g_ptr_array_free(argv, FALSE);
}

gboolean device_match(Device *device, const char *key, char **patterns)
{
    g_auto(GStrv) values = device_values(device, key);
    for (char **pattern = patterns; *pattern; pattern++) {
        g_autofree char *folded_pattern = g_utf8_casefold(*pattern, -1);
        for (char **value = values; value && *value; value++) {
            g_autofree char *folded_value = g_utf8_casefold(*value, -1);
            if (!fnmatch(folded_pattern, folded_value, FNM_NOESCAPE))
                return TRUE;
        }
    }
    return FALSE;
}

static gboolean rule_matches(Config *config, yaml_node_t *rule, Device *device)
{
    if (!rule || rule->type != YAML_MAPPING_NODE)
        return FALSE;
    for (yaml_node_pair_t *pair = rule->data.mapping.pairs.start;
         pair < rule->data.mapping.pairs.top; pair++) {
        const char *key = config_scalar(yaml_document_get_node(&config->document, pair->key));
        if (!key)
            return FALSE;
        if (!strcmp(key, "ignore") || !strcmp(key, "automount") || !strcmp(key, "options") ||
            !strcmp(key, "skip") || !strcmp(key, "keyfile"))
            continue;
        yaml_node_t *node = yaml_document_get_node(&config->document, pair->value);
        g_auto(GStrv) patterns = config_list(config, node);
        if (!device_match(device, key, patterns))
            return FALSE;
    }
    return TRUE;
}

yaml_node_t *device_rule_local(App *app, Device *device, const char *key, gboolean *skip)
{
    Config *config = &app->config;
    yaml_node_t *rules = config_section(config, "device_config");
    *skip = FALSE;
    if (!rules || rules->type != YAML_SEQUENCE_NODE)
        return NULL;
    for (yaml_node_item_t *item = rules->data.sequence.items.start;
         item < rules->data.sequence.items.top; item++) {
        yaml_node_t *rule = yaml_document_get_node(&config->document, *item);
        if (!rule_matches(config, rule, device))
            continue;
        yaml_node_t *value = config_get(config, rule, key);
        if (value)
            return value;
        yaml_node_t *skip_node = config_get(config, rule, "skip");
        const char *scope = config_scalar(skip_node);
        if (config_bool(skip_node, FALSE) || !g_strcmp0(scope, "all") || !g_strcmp0(scope, key)) {
            *skip = TRUE;
            break;
        }
    }
    return NULL;
}

yaml_node_t *device_rule(App *app, Device *device, const char *key)
{
    if (!config_section(&app->config, "device_config"))
        return NULL;
    g_autoptr(Device) parent_device = NULL;
    for (unsigned depth = 0; device && depth < 64; depth++) {
        gboolean skip;
        yaml_node_t *result = device_rule_local(app, device, key, &skip);
        if (result)
            return result;
        /* Partitions and cleartext devices inherit policy from their backing block. */
        const char *parent = device_parent(device);
        if (!parent)
            break;
        Device *next = device_get(app, parent);
        g_clear_pointer(&parent_device, device_free);
        device = parent_device = next;
    }
    return NULL;
}
