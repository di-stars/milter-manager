/* -*- Mode: C; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 *  Copyright (C) 2008  Kouhei Sutou <kou@cozmixng.org>
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
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifndef __MILTER_CLIENT_CONTEXT_H__
#define __MILTER_CLIENT_CONTEXT_H__

#include <glib-object.h>

#include <milter-core.h>

G_BEGIN_DECLS

#define MILTER_CLIENT_CONTEXT_ERROR           (milter_client_context_error_quark())

#define MILTER_CLIENT_TYPE_CONTEXT            (milter_client_context_get_type())
#define MILTER_CLIENT_CONTEXT(obj)            (G_TYPE_CHECK_INSTANCE_CAST((obj), MILTER_CLIENT_TYPE_CONTEXT, MilterClientContext))
#define MILTER_CLIENT_CONTEXT_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST((klass), MILTER_CLIENT_TYPE_CONTEXT, MilterClientContextClass))
#define MILTER_CLIENT_IS_CONTEXT(obj)         (G_TYPE_CHECK_INSTANCE_TYPE((obj), MILTER_CLIENT_TYPE_CONTEXT))
#define MILTER_CLIENT_IS_CONTEXT_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass), MILTER_CLIENT_TYPE_CONTEXT))
#define MILTER_CLIENT_CONTEXT_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS((obj), MILTER_CLIENT_TYPE_CONTEXT, MilterClientContextClass))

typedef enum
{
    MILTER_CLIENT_CONTEXT_ERROR_INVALID_CODE
} MilterClientContextError;

typedef struct _MilterClientContext         MilterClientContext;
typedef struct _MilterClientContextClass    MilterClientContextClass;

struct _MilterClientContext
{
    GObject object;
};

struct _MilterClientContextClass
{
    GObjectClass parent_class;

    MilterStatus (*negotiate)          (MilterClientContext *context,
                                        MilterOption        *option);
    MilterStatus (*connect)            (MilterClientContext *context,
                                        const gchar         *host_name,
                                        struct sockaddr     *address,
                                        socklen_t            address_length);
    MilterStatus (*helo)               (MilterClientContext *context,
                                        const gchar         *fqdn);
    MilterStatus (*envelope_from)      (MilterClientContext *context,
                                        const gchar         *from);
    MilterStatus (*envelope_receipt)   (MilterClientContext *context,
                                        const gchar         *receipt);
    MilterStatus (*data)               (MilterClientContext *context);
    MilterStatus (*unknown)            (MilterClientContext *context,
                                        const gchar         *command);
    MilterStatus (*header)             (MilterClientContext *context,
                                        const gchar         *name,
                                        const gchar         *value);
    MilterStatus (*end_of_header)      (MilterClientContext *context);
    MilterStatus (*body)               (MilterClientContext *context,
                                        const guchar        *chunk,
                                        gsize                size);
    MilterStatus (*end_of_message)     (MilterClientContext *context);
    MilterStatus (*close)              (MilterClientContext *context);
    MilterStatus (*abort)              (MilterClientContext *context);
};

GQuark               milter_client_context_error_quark       (void);

GType                milter_client_context_get_type          (void) G_GNUC_CONST;

MilterClientContext *milter_client_context_new               (void);

gboolean             milter_client_context_feed              (MilterClientContext *context,
                                                              const gchar *chunk,
                                                              gsize size,
                                                              GError **error);

const gchar         *milter_client_context_get_macro         (MilterClientContext *context,
                                                              const gchar *name);
GHashTable          *milter_client_context_get_macros        (MilterClientContext *context);

gpointer             milter_client_context_get_private_data  (MilterClientContext *context);
void                 milter_client_context_set_private_data  (MilterClientContext *context,
                                                              gpointer data,
                                                              GDestroyNotify destroy);

gboolean             milter_client_context_set_reply         (MilterClientContext *context,
                                                              guint code,
                                                              const gchar *extended_code,
                                                              const gchar *message,
                                                              GError **error);
gchar               *milter_client_context_format_reply      (MilterClientContext *context);

gboolean             milter_client_context_add_header        (MilterClientContext *context,
                                                              const gchar *name,
                                                              const gchar *value);
gboolean             milter_client_context_insert_header     (MilterClientContext *context,
                                                              guint32      index,
                                                              const gchar *name,
                                                              const gchar *value);
gboolean             milter_client_context_change_header     (MilterClientContext *context,
                                                              const gchar *name,
                                                              guint32      index,
                                                              const gchar *value);
gboolean             milter_client_context_remove_header     (MilterClientContext *context,
                                                              const gchar *name,
                                                              guint32      index);


void                 milter_client_context_set_writer        (MilterClientContext *context,
                                                              MilterWriter *writer);


G_END_DECLS

#endif /* __MILTER_CLIENT_CONTEXT_H__ */

/*
vi:ts=4:nowrap:ai:expandtab:sw=4
*/
