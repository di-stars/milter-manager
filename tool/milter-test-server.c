/* -*- Mode: C; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 *  Copyright (C) 2008  Kouhei Sutou <kou@cozmixng.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifdef HAVE_CONFIG_H
#  include "../config.h"
#endif /* HAVE_CONFIG_H */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <glib/gstdio.h>
#include <glib/gi18n.h>
#include <fcntl.h>

#include <glib/gprintf.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <milter/server.h>
#include <milter/core.h>

static gboolean verbose = FALSE;
static gboolean output_message = FALSE;
static gchar *spec = NULL;
static gchar *connect_host = NULL;
static struct sockaddr *connect_address = NULL;
static socklen_t connect_address_length = 0;
static gchar *helo_host = NULL;
static gchar *envelope_from = NULL;
static gchar **recipients = NULL;
static gchar **body_chunks = NULL;
static gchar *unknown_command = NULL;
static MilterHeaders *option_headers = NULL;

#define PROGRAM_NAME "milter-test-server"

#define MILTER_TEST_SERVER_ERROR                                \
    (g_quark_from_static_string("milter-test-server-error-quark"))

typedef enum
{
    MILTER_TEST_SERVER_ERROR_INVALID_HEADER,
    MILTER_TEST_SERVER_ERROR_INVALID_MAIL_ADDRESS
} MilterTestServerError;

typedef struct Message
{
    MilterHeaders *headers;
    MilterHeaders *original_headers;
    gchar *envelope_from;
    GList *recipients;
    GString *body_string;
    GString *replaced_body_string;
} Message;

typedef struct _ProcessData
{
    GMainLoop *main_loop;
    GTimer *timer;
    gboolean success;
    guint reply_code;
    gchar *reply_extended_code;
    gchar *reply_message;
    gchar *quarantine_reason;
    MilterStatus status;
    MilterOption *option;
    Message *message;
    GError *error;
} ProcessData;

#define RED_COLOR "\033[01;31m"
#define RED_BACK_COLOR "\033[41m"
#define GREEN_COLOR "\033[01;32m"
#define GREEN_BACK_COLOR "\033[01;42m"
#define YELLOW_COLOR "\033[01;33m"
#define BLUE_COLOR "\033[01;34m"
#define BLUE_BACK_COLOR "\033[01;44m"
#define MAGENTA_COLOR "\033[01;35m"
#define CYAN_COLOR "\033[01;36m"
#define WHITE_COLOR "\033[01;37m"
#define NORMAL_COLOR "\033[00m"

static void
send_quit (MilterServerContext *context, ProcessData *data)
{
    milter_server_context_quit(context);
    milter_agent_shutdown(MILTER_AGENT(context));
}

static void
send_abort (MilterServerContext *context, ProcessData *data)
{
    milter_server_context_abort(context);
    send_quit(context, data);
}

static void
set_macro (gpointer key, gpointer value, gpointer user_data)
{
    MilterProtocolAgent *agent = user_data;
    GList *symbol;
    MilterCommand command = GPOINTER_TO_UINT(key);
    GList *symbols = value;

    for (symbol = symbols; symbol; symbol = g_list_next(symbols)) {
        milter_protocol_agent_set_macro(agent,
                                        command,
                                        symbol->data, symbol->data);
    }
}

static void
send_recipient (MilterServerContext *context)
{
    milter_server_context_envelope_recipient(context, *recipients);
    recipients++;
}

static void
cb_continue (MilterServerContext *context, gpointer user_data)
{
    ProcessData *data = user_data;
    MilterStepFlags step = MILTER_STEP_NONE;

    if (data->option)
        step = milter_option_get_step(data->option);

    switch (milter_server_context_get_state(context)) {
      case MILTER_SERVER_CONTEXT_STATE_NEGOTIATE:
        if (!(step & MILTER_STEP_NO_CONNECT)) {
            const gchar *host_name;

            if (connect_host)
                host_name = connect_host;
            else
                host_name = "mx.local.net";

            if (connect_address) {
                milter_server_context_connect(context,
                                              host_name,
                                              connect_address,
                                              connect_address_length);
            } else {
                struct sockaddr_in address;
                const gchar ip_address[] = "192.168.123.123";
                uint16_t port = 50443;

                address.sin_family = AF_INET;
                address.sin_port = g_htons(port);
                inet_aton(ip_address, &(address.sin_addr));
                milter_server_context_connect(context,
                                              host_name,
                                              (struct sockaddr *)(&address),
                                              sizeof(address));
            }
            break;
        }
      case MILTER_SERVER_CONTEXT_STATE_CONNECT:
        if (!(step & MILTER_STEP_NO_HELO)) {
            milter_server_context_helo(context, helo_host);
            break;
        }
      case MILTER_SERVER_CONTEXT_STATE_HELO:
        if (!(step & MILTER_STEP_NO_ENVELOPE_FROM)) {
            milter_server_context_envelope_from(context, envelope_from);
            break;
        }
      case MILTER_SERVER_CONTEXT_STATE_ENVELOPE_FROM:
        if (!(step & MILTER_STEP_NO_ENVELOPE_RECIPIENT)) {
            send_recipient(context);
            break;
        }
      case MILTER_SERVER_CONTEXT_STATE_ENVELOPE_RECIPIENT:
        if (!(step & MILTER_STEP_NO_ENVELOPE_RECIPIENT) &&
            *recipients) {
            send_recipient(context);
            break;
        }
        if (!(step & MILTER_STEP_NO_UNKNOWN) && unknown_command) {
            milter_server_context_unknown(context, unknown_command);
            break;
        }
      case MILTER_SERVER_CONTEXT_STATE_UNKNOWN:
        if (!(step & MILTER_STEP_NO_DATA)) {
            milter_server_context_data(context);
            break;
        }
      case MILTER_SERVER_CONTEXT_STATE_DATA:
        if (!(step & MILTER_STEP_NO_HEADERS)) {
            MilterHeader *header;
            header = milter_headers_get_nth_header(option_headers, 1);
            milter_server_context_header(context, header->name, header->value);
            milter_headers_remove(option_headers, header);
            break;
        }
      case MILTER_SERVER_CONTEXT_STATE_HEADER:
        if (!(step & MILTER_STEP_NO_HEADERS) &&
            milter_headers_length(option_headers) > 0) {
            MilterHeader *header;
            header = milter_headers_get_nth_header(option_headers, 1);
            milter_server_context_header(context, header->name, header->value);
            milter_headers_remove(option_headers, header);
            break;
        }
        if (!(step & MILTER_STEP_NO_END_OF_HEADER)) {
            milter_server_context_end_of_header(context);
            break;
        }
      case MILTER_SERVER_CONTEXT_STATE_END_OF_HEADER:
        if (!(step & MILTER_STEP_NO_BODY)) {
            milter_server_context_body(context, *body_chunks, strlen(*body_chunks));
            body_chunks++;
            break;
        }
      case MILTER_SERVER_CONTEXT_STATE_BODY:
        if (!(step & MILTER_STEP_NO_BODY) &&
            *body_chunks) {
            milter_server_context_body(context, *body_chunks, strlen(*body_chunks));
            body_chunks++;
            break;
        }
        milter_server_context_end_of_message(context, NULL, 0);
        break;
      case MILTER_SERVER_CONTEXT_STATE_END_OF_MESSAGE:
        send_quit(context, data);
        break;
      default:
        milter_error("Unknown state.");
        send_abort(context, data);
        break;
    }
}

static void
cb_negotiate_reply (MilterServerContext *context, MilterOption *option,
                    MilterMacrosRequests *macros_requests, gpointer user_data)
{
    ProcessData *data = user_data;

    if (data->option) {
        milter_error("duplicated negotiate");
        send_abort(context, data);
    } else {
        data->option = g_object_ref(option);
        if (macros_requests)
            milter_macros_requests_foreach(macros_requests,
                                           (GHFunc)set_macro, context);
        cb_continue(context, user_data);
    }
}

static void
cb_temporary_failure (MilterServerContext *context, gpointer user_data)
{
    ProcessData *data = user_data;
    MilterServerContextState state;

    state = milter_server_context_get_state(context);

    switch (state) {
      case MILTER_SERVER_CONTEXT_STATE_ENVELOPE_RECIPIENT:
        cb_continue(context, user_data);
        break;
      default:
        send_abort(context, data);
        data->success = FALSE;
        data->status = MILTER_STATUS_TEMPORARY_FAILURE;
        break;
    }
}

static void
cb_reject (MilterServerContext *context, gpointer user_data)
{
    ProcessData *data = user_data;
    MilterServerContextState state;

    state = milter_server_context_get_state(context);

    switch (state) {
      case MILTER_SERVER_CONTEXT_STATE_ENVELOPE_RECIPIENT:
        cb_continue(context, user_data);
        break;
      default:
        send_abort(context, data);
        data->success = FALSE;
        data->status = MILTER_STATUS_REJECT;
        break;
    }
}

static void
cb_reply_code (MilterServerContext *context,
               guint code,
               const gchar *extended_code,
               const gchar *message,
               gpointer user_data)
{
    ProcessData *data = user_data;

    data->reply_code = code;
    if (data->reply_extended_code)
        g_free(data->reply_extended_code);
    data->reply_extended_code = g_strdup(extended_code);
    if (data->reply_message)
        g_free(data->reply_message);
    data->reply_message = g_strdup(message);

    cb_continue(context, user_data);
}

static void
cb_accept (MilterServerContext *context, gpointer user_data)
{
    ProcessData *data = user_data;

    send_abort(context, data);
    data->success = TRUE;
    data->status = MILTER_STATUS_ACCEPT;
}

static void
cb_discard (MilterServerContext *context, gpointer user_data)
{
    ProcessData *data = user_data;

    send_abort(context, data);
    data->success = FALSE;
    data->status = MILTER_STATUS_DISCARD;
}

static void
cb_add_header (MilterServerContext *context,
               const gchar *name, const gchar *value,
               gpointer user_data)
{
    ProcessData *data = user_data;

    milter_headers_add_header(data->message->headers, name, value);
}

static void
cb_insert_header (MilterServerContext *context,
                  guint32 index, const gchar *name, const gchar *value,
                  gpointer user_data)
{
    ProcessData *data = user_data;

    milter_headers_insert_header(data->message->headers, index, name, value);
}

static void
cb_change_header (MilterServerContext *context,
                  const gchar *name, guint32 index, const gchar *value,
                  gpointer user_data)
{
    ProcessData *data = user_data;

    milter_headers_change_header(data->message->headers, name, index, value);
}

static void
cb_delete_header (MilterServerContext *context,
                  const gchar *name, guint32 index,
                  gpointer user_data)
{
    ProcessData *data = user_data;

    milter_headers_delete_header(data->message->headers, name, index);
}

static void
cb_change_from (MilterServerContext *context,
                const gchar *from, const gchar *parameters,
                gpointer user_data)
{
    ProcessData *data = user_data;

    if (data->message->envelope_from)
        g_free(data->message->envelope_from);
    data->message->envelope_from = g_strdup(from);
    milter_headers_change_header(data->message->headers, "From", 1, from);
}

static void
cb_add_recipient (MilterServerContext *context,
                  const gchar *recipient, const gchar *parameters,
                  gpointer user_data)
{
    ProcessData *data = user_data;
    MilterHeader *header;
    gchar *old_value;
    
    data->message->recipients = g_list_append(data->message->recipients,
                                              g_strdup(recipient));
    header = milter_headers_lookup_by_name(data->message->headers, "To");
    if (!header) {
        milter_headers_add_header(data->message->headers, "To", recipient);
        return;
    }

    old_value = header->value;
    if (strlen(header->value) == 0)
        header->value = g_strdup(recipient);
    else
        header->value = g_strdup_printf("%s, %s", header->value, recipient);
    g_free(old_value);
}

static void
cb_delete_recipient (MilterServerContext *context, const gchar *recipient,
                     gpointer user_data)
{
    ProcessData *data = user_data;
    MilterHeader *header;
    gchar *old_value;
    
    data->message->recipients = g_list_remove(data->message->recipients,
                                              recipient);

    header = milter_headers_lookup_by_name(data->message->headers, "To");
    if (!header)
        return;

    if (strstr(header->value, recipient)) {
        gchar **recipients, **new_recipients;
        gint i, n_recipients, new_pos;

        recipients = g_strsplit(header->value, ", ", -1);
        n_recipients = g_strv_length(recipients);
        new_recipients = g_new0(gchar*, n_recipients);

        for (i = 0, new_pos = 0; i < n_recipients; i++) {
            if (!strcmp(recipients[i], recipient))
                continue;
            new_recipients[new_pos] = recipients[i];
            new_pos++;
        }
        new_recipients[n_recipients - 1] = NULL;

        g_free(recipients);
        old_value = header->value;
        if (new_pos > 0)
            header->value = g_strjoinv(", ", new_recipients);
        else
            header->value = g_strdup("");
        g_strfreev(new_recipients);
        g_free(old_value);
    }
}

static void
cb_replace_body (MilterServerContext *context,
                 const gchar *body, gsize body_size,
                 gpointer user_data)
{
    ProcessData *data = user_data;

    g_string_append_len(data->message->replaced_body_string, body, body_size);
}

static void
cb_progress (MilterServerContext *context, gpointer user_data)
{
}

static void
cb_quarantine (MilterServerContext *context, const gchar *reason,
              gpointer user_data)
{
    ProcessData *data = user_data;

    data->quarantine_reason = g_strdup(reason);
    send_abort(context, data);
}

static void
cb_connection_failure (MilterServerContext *context, gpointer user_data)
{
    ProcessData *data = user_data;

    send_abort(context, data);
}

static void
cb_shutdown (MilterServerContext *context, gpointer user_data)
{
    ProcessData *data = user_data;

    send_abort(context, data);
}

static void
cb_skip (MilterServerContext *context, gpointer user_data)
{
    while (*body_chunks)
        body_chunks++;

    cb_continue(context, user_data);
}

static void
cb_error (MilterErrorEmittable *emittable, GError *error, gpointer user_data)
{
    ProcessData *data = user_data;

    data->error = g_error_copy(error);

    send_abort(MILTER_SERVER_CONTEXT(emittable), data);
}

static void
cb_finished (MilterFinishedEmittable *emittable, gpointer user_data)
{
    ProcessData *data = user_data;

    g_timer_stop(data->timer);
    g_main_loop_quit(data->main_loop);
}

static void
setup (MilterServerContext *context, ProcessData *data)
{
#define CONNECT(name)                                                   \
    g_signal_connect(context, #name, G_CALLBACK(cb_ ## name), data)

    CONNECT(negotiate_reply);
    CONNECT(continue);
    CONNECT(reply_code);
    CONNECT(temporary_failure);
    CONNECT(reject);
    CONNECT(accept);
    CONNECT(discard);
    CONNECT(add_header);
    CONNECT(insert_header);
    CONNECT(change_header);
    CONNECT(delete_header);
    CONNECT(change_from);
    CONNECT(add_recipient);
    CONNECT(delete_recipient);
    CONNECT(replace_body);
    CONNECT(progress);
    CONNECT(quarantine);
    CONNECT(connection_failure);
    CONNECT(shutdown);
    CONNECT(skip);
    CONNECT(error);
    CONNECT(finished);

#undef CONNECT
}

static void
negotiate (MilterServerContext *context)
{
    MilterOption *option;

    option = milter_option_new(6,
                               MILTER_ACTION_ADD_HEADERS |
                               MILTER_ACTION_CHANGE_BODY |
                               MILTER_ACTION_ADD_ENVELOPE_RECIPIENT |
                               MILTER_ACTION_DELETE_ENVELOPE_RECIPIENT |
                               MILTER_ACTION_CHANGE_HEADERS |
                               MILTER_ACTION_QUARANTINE |
                               MILTER_ACTION_CHANGE_ENVELOPE_FROM |
                               MILTER_ACTION_ADD_ENVELOPE_RECIPIENT_WITH_PARAMETERS |
                               MILTER_ACTION_SET_SYMBOL_LIST,
                               MILTER_STEP_NO_CONNECT |
                               MILTER_STEP_NO_HELO |
                               MILTER_STEP_NO_ENVELOPE_FROM |
                               MILTER_STEP_NO_ENVELOPE_RECIPIENT |
                               MILTER_STEP_NO_BODY |
                               MILTER_STEP_NO_HEADERS |
                               MILTER_STEP_NO_END_OF_HEADER |
                               MILTER_STEP_NO_REPLY_HEADER |
                               MILTER_STEP_NO_UNKNOWN |
                               MILTER_STEP_NO_DATA |
                               MILTER_STEP_SKIP |
                               MILTER_STEP_ENVELOPE_RECIPIENT_REJECTED |
                               MILTER_STEP_NO_REPLY_CONNECT |
                               MILTER_STEP_NO_REPLY_HELO |
                               MILTER_STEP_NO_REPLY_ENVELOPE_FROM |
                               MILTER_STEP_NO_REPLY_ENVELOPE_RECIPIENT |
                               MILTER_STEP_NO_REPLY_DATA |
                               MILTER_STEP_NO_REPLY_UNKNOWN |
                               MILTER_STEP_NO_REPLY_END_OF_HEADER |
                               MILTER_STEP_NO_REPLY_BODY |
                               MILTER_STEP_HEADER_LEAD_SPACE);
    milter_server_context_negotiate(context, option);
    g_object_unref(option);
}

static void
cb_ready (MilterServerContext *context, gpointer user_data)
{
    ProcessData *data = user_data;
    setup(context, data);
    g_timer_start(data->timer);
    negotiate(context);
}

static void
cb_connection_error (MilterErrorEmittable *emittable, GError *error, gpointer user_data)
{
    ProcessData *data = user_data;

    data->success = FALSE;
    data->error = g_error_copy(error);
    g_main_loop_quit(data->main_loop);
}

static gboolean
print_version (const gchar *option_name,
               const gchar *value,
               gpointer data,
               GError **error)
{
    g_printf("%s %s\n", PROGRAM_NAME, VERSION);
    exit(EXIT_SUCCESS);
    return TRUE;
}

static gboolean
parse_spec_arg (const gchar *option_name,
                const gchar *value,
                gpointer data,
                GError **error)
{
    GError *spec_error = NULL;
    gboolean success;

    success = milter_connection_parse_spec(value, NULL, NULL, NULL, &spec_error);
    if (success) {
        spec = g_strdup(value);
    } else {
        g_set_error(error,
                    G_OPTION_ERROR,
                    G_OPTION_ERROR_BAD_VALUE,
                    "%s", spec_error->message);
        g_error_free(spec_error);
    }

    return success;
}

static gboolean
parse_connect_address_arg (const gchar *option_name,
                           const gchar *value,
                           gpointer data,
                           GError **error)
{
    GError *spec_error = NULL;
    gboolean success;

    success = milter_connection_parse_spec(value,
                                           NULL,
                                           &connect_address,
                                           &connect_address_length,
                                           &spec_error);
    if (!success) {
        g_set_error(error,
                    G_OPTION_ERROR,
                    G_OPTION_ERROR_BAD_VALUE,
                    "%s", spec_error->message);
        g_error_free(spec_error);
    }

    return success;
}

static gboolean
parse_header_arg (const gchar *option_name,
                  const gchar *value,
                  gpointer data,
                  GError **error)
{
    gchar **strings;

    strings = g_strsplit(value, ":", 2);
    if (g_strv_length(strings) != 2) {
        g_set_error(error,
                    G_OPTION_ERROR,
                    G_OPTION_ERROR_BAD_VALUE,
                    _("headers option should be 'NAME:VALUE'"));
        g_strfreev(strings);
        return FALSE;
    }

    milter_headers_add_header(option_headers, strings[0], strings[1]);

    g_strfreev(strings);

    return TRUE;
}

static gboolean
set_envelope_from (const gchar *from)
{
    if (envelope_from)
        g_free(envelope_from);
    envelope_from = g_strdup(from);

    return TRUE;
}

static gchar *
extract_path_from_mail_address (const gchar *mail_address, GError **error)
{
    const gchar *path;

    path = strstr(mail_address, "<");
    if (path) {
        const gchar *end_of_path;

        end_of_path = strstr(path, ">");
        if (end_of_path) {
            return g_strndup(path, end_of_path + 1 - path);
        } else {
            g_set_error(error,
                        MILTER_TEST_SERVER_ERROR,
                        MILTER_TEST_SERVER_ERROR_INVALID_MAIL_ADDRESS,
                        "invalid mail address: <%s>", mail_address);
            return NULL;
        }
    } else {
        GString *extracted_path;
        const gchar *end_of_path;

        path = mail_address;
        while (path[0] && path[0] == ' ')
            path++;
        end_of_path = path;
        while (end_of_path[0] && end_of_path[0] != ' ')
            end_of_path++;
        /* FIXME: should check any garbages after end_of_path. */
        extracted_path = g_string_new("<");
        g_string_append_len(extracted_path, path, end_of_path - path);
        g_string_append(extracted_path, ">");
        return g_string_free(extracted_path, FALSE);
    }
}

static gboolean
parse_header (const gchar *line, GList **recipient_list, GError **error)
{
    gchar **strings;

    if (g_str_has_prefix(line, "From: ")) {
        gchar *reverse_path;

        reverse_path = extract_path_from_mail_address(line + strlen("From :"),
                                                      error);
        if (!reverse_path)
            return FALSE;
        set_envelope_from(reverse_path);
        g_free(reverse_path);
    } else if (g_str_has_prefix(line, "To: ")) {
        gchar *forward_path;

        forward_path = extract_path_from_mail_address(line + strlen("To: "),
                                                      error);
        if (!forward_path)
            return FALSE;
        *recipient_list = g_list_append(*recipient_list, forward_path);
    }

    strings = g_strsplit(line, ":", 2);
    milter_headers_add_header(option_headers, strings[0], g_strchug(strings[1]));
    g_strfreev(strings);

    return TRUE;
}

static gboolean
is_header (const gchar *line)
{
    if (!g_ascii_isalnum(line[0]))
        return FALSE;

    if (!strstr(line, ":"))
        return FALSE;
    return TRUE;
}

static void
append_header_value (const gchar *value)
{
    MilterHeader *last_header;
    guint last;
    gchar *old_value;

    last = milter_headers_length(option_headers);
    last_header = milter_headers_get_nth_header(option_headers,
                                                last);

    old_value = last_header->value;
    last_header->value = g_strdup_printf("%s\n%s",
                                         last_header->value,
                                         value);
    g_free(old_value);
}

static gboolean
parse_mail_contents (const gchar *contents, GError **error)
{
    gchar **lines, **first_lines;
    GList *recipient_list = NULL;
    GString *body_string;

    lines = g_strsplit(contents, "\n", -1);
    first_lines = lines;

    /* Ignore mbox separation 'From ' mark. */
    if (g_str_has_prefix(*lines, "From "))
        lines++;

    for (; *lines; lines++) {
        if (*lines[0] == '\0') {
            lines++;
            break;
        } else if (is_header(*lines)) {
            if (!parse_header(*lines, &recipient_list, error)) {
                g_strfreev(first_lines);
                return FALSE;
            }
        } else if (g_ascii_isspace(*lines[0])) {
            append_header_value(*lines);
        } else {
            g_set_error(error,
                        MILTER_TEST_SERVER_ERROR,
                        MILTER_TEST_SERVER_ERROR_INVALID_HEADER,
                        "invalid header: <%s>",
                        *lines);
            g_strfreev(first_lines);
            return FALSE;
        }
    }

    body_string = g_string_new(NULL);
    for (; *lines; lines++) {
        g_string_append_printf(body_string, "%s\r\n", *lines);
    }

    if (recipient_list) {
        gint i, length;
        GList *node;

        length = g_list_length(recipient_list);
        recipients = g_new0(gchar *, length + 1);
        for (i = 0, node = recipient_list; node; i++, node = g_list_next(node)) {
            recipients[i] = node->data;
        }
        recipients[length] = NULL;
        g_list_free(recipient_list);
    }

    if (body_string->len > 0) {
        g_string_truncate(body_string, body_string->len - strlen("\r\n"));
        body_chunks = g_new0(gchar*, 2);
        body_chunks[0] = g_strdup(body_string->str);
        body_chunks[1] = NULL;
    }
    g_string_free(body_string, TRUE);

    g_strfreev(first_lines);

    return TRUE;
}

static gboolean
parse_mail_file_arg (const gchar *option_name,
                     const gchar *value,
                     gpointer data,
                     GError **error)
{
    gchar *contents = NULL;
    gsize length;
    GError *internal_error = NULL;
    GIOChannel *io_channel;

    if (g_str_equal(value, "-")) {
        io_channel = g_io_channel_unix_new(STDIN_FILENO);
    } else {
        if (!g_file_test(value, G_FILE_TEST_EXISTS)) {
            g_set_error(error,
                    G_OPTION_ERROR,
                    G_OPTION_ERROR_BAD_VALUE,
                    _("%s does not exist."), value);
            return FALSE;
        }
        io_channel = g_io_channel_new_file(value, "r", &internal_error);
        if (!io_channel) {
            g_set_error(error,
                        G_OPTION_ERROR,
                        G_OPTION_ERROR_FAILED,
                        _("Loading from %s failed.: %s"),
                        value, internal_error->message);
            g_error_free(internal_error);
            return FALSE;
        }
    }

    g_io_channel_set_encoding(io_channel, NULL, NULL);
    if (g_io_channel_read_to_end(io_channel, &contents, &length, &internal_error) != G_IO_STATUS_NORMAL) {
        g_set_error(error,
                    G_OPTION_ERROR,
                    G_OPTION_ERROR_FAILED,
                    _("Loading from %s failed.: %s"),
                    value, internal_error->message);
        g_error_free(internal_error);
        g_io_channel_unref(io_channel);
        return FALSE;
    }
    g_io_channel_unref(io_channel);

    if (!parse_mail_contents(contents, &internal_error)) {
        g_set_error(error,
                    G_OPTION_ERROR,
                    G_OPTION_ERROR_FAILED,
                    "%s", internal_error->message);
        g_error_free(internal_error);
        return FALSE;
    }

    g_free(contents);
    return TRUE;
}

static const GOptionEntry option_entries[] =
{
    {"connection-spec", 's', 0, G_OPTION_ARG_CALLBACK, parse_spec_arg,
     N_("The spec of client socket. "
        "(unix:PATH|inet:PORT[@HOST]|inet6:PORT[@HOST])"),
     "SPEC"},
    {"connect-host", 0, 0, G_OPTION_ARG_STRING, &connect_host,
     N_("Use HOST as host name on connect"), "HOST"},
    {"connect-address", 0, 0, G_OPTION_ARG_CALLBACK, parse_connect_address_arg,
     N_("Use SPEC for address on connect. "
        "(unix:PATH|inet:PORT[@HOST]|inet6:PORT[@HOST])"),
     "SPEC"},
    {"helo-fqdn", 0, 0, G_OPTION_ARG_STRING, &helo_host,
     N_("Use FQDN for HELO/EHLO command"), "FQDN"},
    {"from", 'f', 0, G_OPTION_ARG_STRING, &envelope_from,
     N_("Use a sender address"), "FROM"},
    {"recipient", 'r', 0, G_OPTION_ARG_STRING_ARRAY, &recipients,
     N_("Add a recipient. To add n recipients, use --recipient option n times."),
     "RECIPIENT"},
    {"header", 'h', 0, G_OPTION_ARG_CALLBACK, parse_header_arg,
     N_("Add a header. To add n headers, use --header option n times."),
     "NAME:VALUE"},
    {"body", 'b', 0, G_OPTION_ARG_STRING_ARRAY, &body_chunks,
     N_("Add a body chunk. To add n body chunks, use --body option n times."),
     "CHUNK"},
    {"unknown", 0, 0, G_OPTION_ARG_STRING, &unknown_command,
     N_("Use COMMAND for unknown SMTP command."), "COMMAND"},
    {"mail-file", 'm', 0, G_OPTION_ARG_CALLBACK, parse_mail_file_arg,
     N_("Use mail placed at PATH as mail content."), "PATH"},
    {"output-message", 0,
     G_OPTION_FLAG_NO_ARG, G_OPTION_ARG_NONE, &output_message,
     N_("Output modified message"), NULL},
    {"verbose", 'v', G_OPTION_FLAG_NO_ARG, G_OPTION_ARG_NONE, &verbose,
     N_("Be verbose"), NULL},
    {"version", 0, G_OPTION_FLAG_NO_ARG, G_OPTION_ARG_CALLBACK, print_version,
     N_("Show version"), NULL},
    {NULL}
};

static Message *
message_new (void)
{
    Message *message;
    gint i;

    message = g_new0(Message, 1);
    message->envelope_from = g_strdup(envelope_from);
    for (i = 0; i < g_strv_length(recipients); i++)
        message->recipients = g_list_append(message->recipients, g_strdup(recipients[i]));
    message->headers = milter_headers_copy(option_headers);
    message->original_headers = milter_headers_copy(option_headers);

    message->body_string = g_string_new(NULL);
    for (i = 0; i < g_strv_length(body_chunks); i++)
        g_string_append(message->body_string, g_strdup(body_chunks[i]));
    message->replaced_body_string = g_string_new(NULL);

    return message;
}

static void
free_message (Message *message)
{
    if (message->envelope_from)
        g_free(message->envelope_from);
    if (message->recipients) {
        g_list_foreach(message->recipients, (GFunc)g_free, NULL);
        g_list_free(message->recipients);
    }
    if (message->headers)
        g_object_unref(message->headers);
    if (message->original_headers)
        g_object_unref(message->original_headers);
    if (message->body_string)
        g_string_free(message->body_string, TRUE);

    g_free(message);
}

static void
init_process_data (ProcessData *data)
{
    data->main_loop = g_main_loop_new(NULL, FALSE);
    data->timer = g_timer_new();
    data->success = TRUE;
    data->status = MILTER_STATUS_NOT_CHANGE;
    data->quarantine_reason = NULL;
    data->option = NULL;
    data->reply_code = 0;
    data->reply_extended_code = NULL;
    data->reply_message = NULL;
    data->message = message_new();
    data->error = NULL;
}

static void
free_process_data (ProcessData *data)
{
    if (data->timer)
        g_timer_destroy(data->timer);
    if (data->option)
        g_object_unref(data->option);
    if (data->reply_extended_code)
        g_free(data->reply_extended_code);
    if (data->reply_message)
        g_free(data->reply_message);
    if (data->quarantine_reason)
        g_free(data->quarantine_reason);
    if (data->message)
        free_message(data->message);
    if (data->error)
        g_error_free(data->error);
}

static gboolean
pre_option_parse (GOptionContext *option_context,
                  GOptionGroup *option_group,
                  gpointer data,
                  GError **error)
{
    option_headers = milter_headers_new();

    return TRUE;
}

static gboolean
post_option_parse (GOptionContext *option_context,
                   GOptionGroup *option_group,
                   gpointer data,
                   GError **error)
{
    if (!helo_host)
        helo_host = g_strdup("delian");
    if (!envelope_from)
        envelope_from = g_strdup("<kou+send@cozmixng.org>");
    if (!recipients)
        recipients = g_strsplit("<kou+receive@cozmixng.org>", ",", -1);
    if (!milter_headers_lookup_by_name(option_headers, "From"))
        milter_headers_add_header(option_headers, "From", envelope_from);
    if (!milter_headers_lookup_by_name(option_headers, "To"))
        milter_headers_add_header(option_headers, "To", recipients[0]);
    if (!body_chunks) {
        const gchar body[] =
            "La de da de da 1.\n"
            "La de da de da 2.\n"
            "La de da de da 3.\n"
            "La de da de da 4.";
        body_chunks = g_strsplit(body, ",", -1);
    }

    return TRUE;
}

static void
free_option_values (void)
{
    if (option_headers)
        g_object_unref(option_headers);
}

static void
print_header (const gchar *prefix, MilterHeader *header)
{
    gchar **value_lines, **first_line;
    const gchar *color = NORMAL_COLOR;

    if (g_str_has_prefix(prefix, "+"))
        color = GREEN_COLOR;
    else if (g_str_has_prefix(prefix, "-"))
        color = RED_COLOR;

    value_lines = g_strsplit(header->value, "\n", -1);
    first_line = value_lines;

    if (!*value_lines) {
        g_printf("%s%s %s:%s\n", color, prefix, header->name, NORMAL_COLOR);
        g_strfreev(value_lines);
        return;
    }

    g_printf("%s%s %s: %s%s\n", color, prefix, header->name, *value_lines, NORMAL_COLOR);
    value_lines++;

    while (*value_lines) {
        g_printf("%s%s %s%s\n", color, prefix, *value_lines, NORMAL_COLOR);
        value_lines++;
    }

    g_strfreev(first_line);
}

static void
print_headers (Message *message)
{
    const GList *node, *result_node;

    result_node = milter_headers_get_list(message->headers);

    node = milter_headers_get_list(message->original_headers);
    while (node && result_node) {
        MilterHeader *original_header = node->data;
        MilterHeader *result_header = result_node->data;

        if (milter_header_equal(original_header, result_header)) {
            print_header(" ", original_header);
            result_node = g_list_next(result_node);
            node = g_list_next(node);
        } else if (g_str_equal(original_header->name, result_header->name)){
            print_header("-", original_header);
            print_header("+", result_header);
            result_node = g_list_next(result_node);
            node = g_list_next(node);
        } else if (!g_list_find_custom((GList*)result_node,
                                       original_header,
                                       (GCompareFunc)milter_header_compare)){
            print_header("-", original_header);
            node = g_list_next(node);
        } else {
            print_header("+", result_header);
            result_node = g_list_next(result_node);
        }
    }

    while (node) {
        MilterHeader *header = node->data;
        print_header("-", header);
        node = g_list_next(node);
    }

    for (node = result_node; node; node = g_list_next(node)) {
        MilterHeader *header = node->data;
        print_header("+", header);
    }
}

static gchar *
get_charset (const gchar *value)
{
    gchar *end_pos, *pos;

    pos = strstr(value, "charset=");
    if (!pos)
        return NULL;

    pos += strlen("charset=");
    
    if (pos[0] == '"' || pos[0] == '\'') {
        end_pos = strchr(pos + 1, pos[0]);
        if (!end_pos)
            return NULL;
        return g_strndup(pos + 1, end_pos - pos);
    }

    end_pos = strchr(pos, ';');
    if (!end_pos)
        return g_strdup(pos);

    return g_strndup(pos, end_pos - pos);
}

static gchar *
get_message_charset (Message *message)
{
    MilterHeader *header;

    header = milter_headers_lookup_by_name(message->headers,
                                           "Content-Type");
    if (!header)
        return NULL;

    return get_charset(header->value);
}

static void
print_body (Message *message)
{
    gchar *charset = NULL;
    gchar *body_string;
    gssize body_size;

    if (message->replaced_body_string->len > 0) {
        body_string = message->replaced_body_string->str;
        body_size = message->replaced_body_string->len;
    }else {
        body_string = message->body_string->str;
        body_size = message->body_string->len;
    }

    charset = get_message_charset(message);
    if (charset) {
        gchar *translated_body;
        gsize bytes_read, bytes_written;
        GError *error = NULL;

        translated_body = g_convert(body_string,
                                    body_size,
                                    "UTF-8",
                                    charset,
                                    &bytes_read,
                                    &bytes_written,
                                    &error);
        if (!translated_body)
            g_printf("%s\n", body_string);
        else
            g_printf("%s\n", translated_body);

        if (charset)
            g_free(charset);
    } else {
        g_printf("%s\n", body_string);
    }
}

static void
print_message (Message *message)
{
    print_headers(message);
    g_printf("---------------------------------------\n");
    print_body(message);
}

static void
print_status (ProcessData *data)
{
    GEnumValue *value;
    GEnumClass *enum_class;

    enum_class = g_type_class_ref(MILTER_TYPE_STATUS);
    value = g_enum_get_value(enum_class, data->status);
    g_type_class_unref(enum_class);
    g_printf("The message was '%s'.\n", value->value_nick);
}

static void
print_result (ProcessData *data)
{
    if (data->error) {
        g_printf("%s\n", data->error->message);
        return;
    }

    print_status(data);
    if (data->quarantine_reason) {
        g_printf("The message was quarantined.: %s\n",
                 data->quarantine_reason);
    }

    if (output_message) {
        g_print("\n");
        print_message(data->message);
        g_print("\n");
    }

    g_print("Finished in %gsec.\n", g_timer_elapsed(data->timer, NULL));
}

static void
setup_default_macros (MilterServerContext *context)
{
    MilterProtocolAgent *agent = MILTER_PROTOCOL_AGENT(context);
    gchar *recipients_address;

    milter_protocol_agent_set_macros(agent, MILTER_COMMAND_CONNECT,
                                     "{daemon_name}", PROGRAM_NAME,
                                     "{if_name}", "localhost",
                                     "{if_addr}", "127.0.0.1",
                                     "j", "milter-test-server",
                                     NULL);
    milter_protocol_agent_set_macros(agent, MILTER_COMMAND_HELO,
                                     "{tls_version}", "0",
                                     "{cipher}", "0",
                                     "{cipher_bits}", "0",
                                     "{cert_subject}", "cert_subject",
                                     "{cert_issuer}", "cert_issuer",
                                     NULL);

    milter_protocol_agent_set_macros(agent, MILTER_COMMAND_ENVELOPE_FROM,
                                     "i", "i",
                                     "{auth_type}", "auth_type",
                                     "{auth_authen}", "auth_authen",
                                     "{auto_ssf}", "auto_ssf",
                                     "{auto_author}", "auto_author",
                                     "{mail_mailer}", "mail_mailer",
                                     "{mail_host}", "mail_host",
                                     "{mail_addr}", "mail_addr",
                                     NULL);

    recipients_address = g_strjoinv(",", recipients);
    milter_protocol_agent_set_macros(agent, MILTER_COMMAND_ENVELOPE_RECIPIENT,
                                     "{rcpt_mailer}", "rcpt_mailer",
                                     "{rcpt_host}", "rcpt_host",
                                     "{rcpt_addr}", recipients_address,
                                     NULL);
    g_free(recipients_address);
    milter_protocol_agent_set_macros(agent, MILTER_COMMAND_END_OF_MESSAGE,
                                     "{msg-id}", "msg-id",
                                     NULL);
}

static void
setup_context (MilterServerContext *context, ProcessData *process_data)
{
    setup_default_macros(context);
    milter_server_context_set_name(context, PROGRAM_NAME);

    g_signal_connect(context, "ready", G_CALLBACK(cb_ready), process_data);
    g_signal_connect(context, "error", G_CALLBACK(cb_connection_error), process_data);
}

static gboolean
start_process (MilterServerContext *context, ProcessData *process_data)
{
    gboolean success;
    GError *error = NULL;

    success = milter_server_context_set_connection_spec(context, spec, &error);
    if (success)
        success = milter_server_context_establish_connection(context, &error);

    if (!success) {
        g_printf("%s\n", error->message);
        g_error_free(error);
        return FALSE;
    }

    g_main_loop_run(process_data->main_loop);

    print_result(process_data);

    return TRUE;
}

int
main (int argc, char *argv[])
{
    gboolean success = TRUE;
    MilterServerContext *context;
    GError *error = NULL;
    GOptionContext *option_context;
    GOptionGroup *main_group;
    ProcessData process_data;

    milter_init();

    option_context = g_option_context_new(NULL);
    g_option_context_add_main_entries(option_context, option_entries, NULL);
    main_group = g_option_context_get_main_group(option_context);
    g_option_group_set_parse_hooks(main_group,
                                   pre_option_parse,
                                   post_option_parse);

    if (!g_option_context_parse(option_context, &argc, &argv, &error)) {
        g_printf("%s\n", error->message);
        g_error_free(error);
        g_option_context_free(option_context);
        free_option_values();
        exit(EXIT_FAILURE);
    }

    if (verbose)
        g_setenv("MILTER_LOG_LEVEL", "all", FALSE);

    context = milter_server_context_new();
    init_process_data(&process_data);
    setup_context(context, &process_data);

    success = start_process(context, &process_data);

    g_object_unref(context);
    free_process_data(&process_data);

    milter_quit();
    g_option_context_free(option_context);
    free_option_values();

    exit(success ? EXIT_SUCCESS : EXIT_FAILURE);
}

/*
vi:ts=4:nowrap:ai:expandtab:sw=4
*/
