/* -*- Mode: C; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 *  Copyright (C) 2008-2010  Kouhei Sutou <kou@cozmixng.org>
 *
 *  This library is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with this library.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifdef HAVE_CONFIG_H
#  include "../../config.h"
#endif /* HAVE_CONFIG_H */

#include <errno.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <sys/stat.h>

#include <signal.h>

#include <gcutter.h>
#include "milter-test-utils.h"
#include "milter-manager-test-utils.h"
#include "milter-manager-test-scenario.h"

void test_version (void);
void test_invalid_spec (void);
void test_unknown_option (void);
void test_check_controller_port (void);
void test_unix_socket_mode (void);
void test_remove_manager_unix_socket_on_close (void);

void data_scenario (void);
void test_scenario (gconstpointer data);

static gchar *scenario_dir;
static MilterManagerTestScenario *main_scenario;

static gchar *milter_manager_program_name;

static gchar *original_lang;

static gchar *tmp_dir;

typedef struct _EggData
{
    GCutEgg *egg;
    gchar *command_path;
    GString *output_string;
    GString *error_string;
    gboolean reaped;
} EggData;

static EggData *manager_data;
static EggData *server_data;

static gchar *
build_manager_path (void)
{
    return g_build_filename(milter_test_get_base_dir(),
                            "..",
                            "src",
                            "milter-manager",
                            NULL);
}

static gchar *
build_server_path (void)
{
    return g_build_filename(milter_test_get_base_dir(),
                            "..",
                            "tool",
                            "milter-test-server",
                            NULL);
}

typedef gchar *(*BuildPathFunc) (void);

static EggData *
egg_data_new (BuildPathFunc path_func)
{
    EggData *data;

    data = g_new0(EggData, 1);
    data->egg = NULL;
    data->command_path = path_func();
    data->output_string = g_string_new(NULL);
    data->error_string = g_string_new(NULL);
    data->reaped = FALSE;

    return data;
}

static gboolean
cb_timeout_emitted (gpointer data)
{
    gboolean *timeout_emitted = data;

    *timeout_emitted = TRUE;
    return FALSE;
}

#define manager_egg manager_data->egg
#define server_egg server_data->egg

#define wait_for_server_reaping()               \
    cut_trace(wait_for_reaping(server_data, TRUE))

#define wait_for_manager_reaping()              \
    cut_trace(wait_for_reaping(manager_data, TRUE))

static void
wait_for_reaping (EggData *data, gboolean must)
{
    gboolean timeout_emitted = FALSE;
    guint timeout_id;

    timeout_id = g_timeout_add(2000, cb_timeout_emitted, &timeout_emitted);
    while (!timeout_emitted && !data->reaped)
        g_main_context_iteration(NULL, TRUE);
    g_source_remove(timeout_id);

    if (must) {
        cut_set_message("OUTPUT: <%s>\n"
                        "ERROR: <%s>",
                        data->output_string->str,
                        data->error_string->str);
        cut_assert_false(timeout_emitted);
    }
}

static void
egg_data_free (EggData *data)
{
    if (data->egg) {
        if (!data->reaped && gcut_egg_get_pid(data->egg) > 0)
            gcut_egg_kill(data->egg, SIGINT);
        wait_for_reaping(manager_data, FALSE);
        g_object_unref(data->egg);
    }

    if (data->command_path)
        g_free(data->command_path);
    if (data->output_string)
        g_string_free(data->output_string, TRUE);
    if (data->error_string)
        g_string_free(data->error_string, TRUE);
    g_free(data);
}

void
cut_setup (void)
{
    gchar *lt_milter_manager;

    original_lang = g_strdup(g_getenv("LANG"));
    g_setenv("LANG", "C", TRUE);

    scenario_dir = g_build_filename(milter_test_get_base_dir(),
                                    "fixtures",
                                    "manager",
                                    NULL);

    main_scenario = NULL;

    manager_data = egg_data_new(build_manager_path);
    server_data = egg_data_new(build_server_path);

    lt_milter_manager = g_build_filename(milter_test_get_base_dir(),
                                         "..",
                                         "src",
                                         ".libs",
                                         "lt-milter-manager",
                                         NULL);
    if (g_file_test(lt_milter_manager, G_FILE_TEST_EXISTS))
        milter_manager_program_name = g_strdup("lt-milter-manager");
    else
        milter_manager_program_name = g_strdup("milter-manager");
    g_free(lt_milter_manager);

    tmp_dir = g_build_filename(milter_test_get_base_dir(),
                               "tmp",
                               NULL);
    cut_remove_path(tmp_dir, NULL);
    if (g_mkdir_with_parents(tmp_dir, 0700) == -1)
        cut_assert_errno();
}

void
cut_teardown (void)
{
    if (scenario_dir)
        g_free(scenario_dir);
    if (main_scenario)
        g_object_unref(main_scenario);

    if (manager_data)
        egg_data_free(manager_data);
    if (server_data)
        egg_data_free(server_data);

    if (milter_manager_program_name)
        g_free(milter_manager_program_name);

    if (tmp_dir) {
        cut_remove_path(tmp_dir, NULL);
        g_free(tmp_dir);
    }

    if (original_lang) {
        g_setenv("LANG", original_lang, TRUE);
        g_free(original_lang);
    } else {
        g_unsetenv("LANG");
    }
}

static void
cb_output_received (GCutEgg *egg, const gchar *chunk, gsize size,
                    gpointer user_data)
{
    EggData *data = user_data;
    g_string_append_len(data->output_string, chunk, size);
}

static void
cb_error_received (GCutEgg *egg, const gchar *chunk, gsize size,
                   gpointer user_data)
{
    EggData *data = user_data;
    g_string_append_len(data->error_string, chunk, size);
}

static void
cb_reaped (GCutEgg *egg, gint status, gpointer user_data)
{
    EggData *data = user_data;
    data->reaped = TRUE;
}

static void
setup_egg (EggData *data, const gchar *first_arg, ...)
{
    va_list var_args;
    const gchar *arg;
    gint argc = 1, i;
    gchar **argv;
    GList *strings = NULL, *node;

    va_start(var_args, first_arg);
    arg = first_arg;
    while (arg) {
        argc++;
        strings = g_list_append(strings, g_strdup(arg));
        arg = va_arg(var_args, gchar *);
    }
    va_end(var_args);

    argv = g_new0(gchar *, argc + 1);
    argv[0] = g_strdup(data->command_path);
    argv[argc] = NULL;
    for (node = strings, i = 1; node; node = g_list_next(node), i++) {
        argv[i] = node->data;
    }
    if (strings)
        g_list_free(strings);

    data->egg = gcut_egg_new_argv(argc, argv);
    g_strfreev(argv);

#define CONNECT(name)                                                   \
    g_signal_connect(data->egg, #name, G_CALLBACK(cb_ ## name), data)

    CONNECT(output_received);
    CONNECT(error_received);
    CONNECT(reaped);
#undef CONNECT
}

static void
wait_for_manager_ready (const gchar *spec)
{
    gboolean timeout_emitted = FALSE;
    guint timeout_waiting_id;
    gint socket_fd = -1;
    gint domain;
    struct sockaddr *address;
    socklen_t address_size;
    gint errno_keep = 0;
    GError *error = NULL;

    milter_connection_parse_spec(spec,
                                 &domain,
                                 &address,
                                 &address_size,
                                 &error);
    gcut_assert_error(error);

    timeout_waiting_id = g_timeout_add(1000, cb_timeout_emitted,
                                       &timeout_emitted);
    do {
        if (socket_fd != -1)
            close(socket_fd);
        socket_fd = socket(domain, SOCK_STREAM, 0);
        if (socket_fd == -1)
            continue;

        if (connect(socket_fd, address, address_size) == 0) {
            errno_keep = 0;
            break;
        } else {
            errno_keep = errno;
        }
        g_main_context_iteration(NULL, FALSE);
    } while (!timeout_emitted);
    g_source_remove(timeout_waiting_id);
    if (socket_fd != -1)
        close(socket_fd);

    errno = errno_keep;
    cut_assert_errno(cut_message("spec: <%s>", spec));

    cut_assert_false(timeout_emitted);
}

static void
start_manager (void)
{
    GError *error = NULL;
    const gchar spec[] = "inet:19999@localhost";

    setup_egg(manager_data, "-s", spec, "--config-dir", scenario_dir, NULL);
    gcut_egg_hatch(manager_egg, &error);
    gcut_assert_error(error);
    cut_trace(wait_for_manager_ready(spec));

    cut_assert_equal_string("", manager_data->output_string->str);
    cut_assert_equal_string("", manager_data->error_string->str);
}

static void
start_server (void)
{
    GError *error = NULL;
    const gchar spec[] = "inet:19999@localhost";

    setup_egg(server_data, "--unknown='!'", "-s", spec, NULL);
    gcut_egg_hatch(server_egg, &error);
    gcut_assert_error(error);
}

void
test_version (void)
{
    GError *error = NULL;

    setup_egg(manager_data, "--version", NULL);
    gcut_egg_hatch(manager_egg, &error);
    gcut_assert_error(error);

    wait_for_manager_reaping();

    cut_assert_equal_string("milter-manager " VERSION "\n",
                            manager_data->output_string->str);
}

void
test_invalid_spec (void)
{
    GError *error = NULL;

    setup_egg(manager_data, "-s", "XXXX@localhost", NULL);
    gcut_egg_hatch(manager_egg, &error);
    gcut_assert_error(error);

    wait_for_manager_reaping();

    cut_assert_equal_string("invalid connection spec: "
                            "spec doesn't have colon: <XXXX@localhost>\n",
                            manager_data->output_string->str);
}

void
test_unknown_option (void)
{
    GError *error = NULL;

    setup_egg(manager_data, "--nonexistent", NULL);
    gcut_egg_hatch(manager_egg, &error);
    gcut_assert_error(error);

    wait_for_manager_reaping();

    cut_assert_equal_string(
        cut_take_printf(
            "Unknown option --nonexistent\n"
            "\n"
            "Usage:\n"
            "  %s [OPTION...]\n"
            "\n"
            "Help Options:\n"
#if GLIB_CHECK_VERSION(2, 21, 0)
            "  -h, --help                     Show help options\n"
#else
            "  -?, --help                     Show help options\n"
#endif
            "\n"
            "Application Options:\n"
            "  -s, --connection-spec=SPEC     The spec of socket. (unix:PATH|inet:PORT[@HOST]|inet6:PORT[@HOST])\n"
            "  -c, --config-dir=DIRECTORY     The configuration directory that has configuration file.\n"
            "  --pid-file=FILE                The file name to be saved PID.\n"
            "  -u, --user-name=NAME           The user name for running milter-manager.\n"
            "  -g, --group-name=NAME          The group name for running milter-manager.\n"
            "  --socket-group-name=NAME       The group name for UNIX domain socket.\n"
            "  --daemon                       Run as daemon process.\n"
            "  --no-daemon                    Cancel the prior --daemon options.\n"
            "  --show-config                  Show configuration and exit\n"
            "  --verbose                      Be verbose\n"
            "  --version                      Show version\n"
            "\n",
            milter_manager_program_name),
        manager_data->output_string->str);
}

#define has_key(scenario, group, key)                                     \
    milter_manager_test_scenario_has_key(scenario, group, key)

#define get_integer(scenario, group, key)                               \
    milter_manager_test_scenario_get_integer(scenario, group, key)

#define get_boolean(scenario, group, key)                               \
    milter_manager_test_scenario_get_boolean(scenario, group, key)

#define get_string(scenario, group, key)                                \
    milter_manager_test_scenario_get_string(scenario, group, key)

#define get_string_with_sub_key(scenario, group, key, sub_key)          \
    milter_manager_test_scenario_get_string_with_sub_key(scenario, group, key, sub_key)

#define get_string_list(scenario, group, key, length)                   \
    milter_manager_test_scenario_get_string_list(scenario, group, key, length)

#define get_string_g_list(scenario, group, key)                         \
    milter_manager_test_scenario_get_string_g_list(scenario, group, key)

#define get_enum(scenario, group, key, enum_type)                       \
    milter_manager_test_scenario_get_enum(scenario, group, key, enum_type)

#define get_option_list(scenario, group, key)                           \
    milter_manager_test_scenario_get_option_list(scenario, group, key)

#define get_header_list(scenario, group, key)                           \
    milter_manager_test_scenario_get_header_list(scenario, group, key)

#define get_pair_list(scenario, group, key)                             \
    milter_manager_test_scenario_get_pair_list(scenario, group, key)

#define get_pair_list_without_sort(scenario, group, key)                         \
    milter_manager_test_scenario_get_pair_list_without_sort(scenario, group, key)

static void
do_actions (MilterManagerTestScenario *scenario)
{
    const gchar *expected;
    gchar **lines;

    wait_for_server_reaping();
    cut_assert_equal_string("", server_data->error_string->str);

    expected = get_string(scenario, "scenario", "expected");
    lines = g_strsplit(server_data->output_string->str, "\n", 2);
    cut_take_string_array(lines);
    /* g_print("%s", server_data->output_string->str); */
    cut_assert_equal_string(expected, lines[0]);
    cut_assert_match("^elapsed-time: [\\d.]+ seconds$", lines[1]);
}

void
data_scenario (void)
{
    cut_add_data("normal",
                 g_strdup("normal.txt"), g_free);
    cut_add_data("reject on envelope-from",
                 g_strdup("reject-on-envelope-from.txt"), g_free);
    cut_add_data("reject on envelope-recipient",
                 g_strdup("reject-on-envelope-recipient.txt"), g_free);
    cut_add_data("skip on body",
                 g_strdup("skip-on-body.txt"), g_free);
}

void
test_scenario (gconstpointer data)
{
    const gchar *scenario_name = data;
    const gchar omit_key[] = "omit";

    start_manager();

    main_scenario = milter_manager_test_scenario_new();
    cut_trace(milter_manager_test_scenario_load(main_scenario,
                                                scenario_dir,
                                                scenario_name));

    if (has_key(main_scenario,
                MILTER_MANAGER_TEST_SCENARIO_GROUP_NAME, omit_key))
        cut_omit("%s", get_string(main_scenario,
                                  MILTER_MANAGER_TEST_SCENARIO_GROUP_NAME,
                                  omit_key));

    cut_trace(milter_manager_test_scenario_start_clients(main_scenario));

    start_server();

    cut_trace(do_actions(main_scenario));
    wait_for_server_reaping();
}

void
test_check_controller_port (void)
{
    GError *error = NULL;

    setup_egg(manager_data, "--config-dir", scenario_dir, NULL);
    gcut_egg_hatch(manager_egg, &error);
    gcut_assert_error(error);
    cut_trace(wait_for_manager_ready("inet:2929@localhost"));
}

void
test_unix_socket_mode (void)
{
    GError *error = NULL;
    const gchar *path;
    const gchar *spec;
    struct stat stat_buffer;

    path = cut_take_printf("%s/milter.sock", tmp_dir);
    spec = cut_take_printf("unix:%s", path);
    setup_egg(manager_data, "-s", spec, NULL);
    gcut_egg_hatch(manager_egg, &error);
    gcut_assert_error(error);

    cut_trace(wait_for_manager_ready(spec));

    if (stat(path, &stat_buffer) == -1)
        cut_assert_errno();

    cut_assert_equal_uint(S_IFSOCK | 0660, stat_buffer.st_mode);
}

void
test_remove_manager_unix_socket_on_close (void)
{
    GError *error = NULL;
    const gchar *path;
    const gchar *spec;

    path = cut_take_printf("%s/milter.sock", tmp_dir);
    cut_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));

    spec = cut_take_printf("unix:%s", path);
    setup_egg(manager_data,
              "-s", spec,
              "--config-dir", scenario_dir, NULL);
    gcut_egg_hatch(manager_egg, &error);
    gcut_assert_error(error);

    cut_trace(wait_for_manager_ready(spec));
    cut_assert_true(g_file_test(path, G_FILE_TEST_EXISTS));

    gcut_egg_kill(manager_egg, SIGINT);
    cut_trace(wait_for_reaping(manager_data, TRUE));
    cut_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
}

/*
vi:ts=4:nowrap:ai:expandtab:sw=4
*/
