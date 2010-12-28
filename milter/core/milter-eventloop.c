/* -*- Mode: C; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 *  Copyright (C) 2008-2010  Nobuyoshi Nakada <nakada@clear-code.com>
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

#include "milter-eventloop.h"

#define MILTER_EVENTLOOP_GET_PRIVATE(obj)               \
  (G_TYPE_INSTANCE_GET_PRIVATE((obj),                   \
                               MILTER_TYPE_EVENTLOOP,   \
                               MilterEventloopPrivate))

G_DEFINE_ABSTRACT_TYPE(MilterEventloop, milter_eventloop, G_TYPE_OBJECT)

typedef struct _MilterEventloopPrivate	MilterEventloopPrivate;
struct _MilterEventloopPrivate
{
  gint dummy;
};

enum
{
    PROP_0,
    PROP_LAST
};

static GObject *constructor  (GType                  type,
                              guint                  n_props,
                              GObjectConstructParam *props);

static void dispose        (GObject         *object);
static void set_property   (GObject         *object,
                            guint            prop_id,
                            const GValue    *value,
                            GParamSpec      *pspec);
static void get_property   (GObject         *object,
                            guint            prop_id,
                            GValue          *value,
                            GParamSpec      *pspec);

static void
milter_eventloop_class_init (MilterEventloopClass *klass)
{
    GObjectClass *gobject_class;

    gobject_class = G_OBJECT_CLASS(klass);

    gobject_class->constructor  = constructor;
    gobject_class->dispose      = dispose;
    gobject_class->set_property = set_property;
    gobject_class->get_property = get_property;

    g_type_class_add_private(gobject_class, sizeof(MilterEventloopPrivate));
}

static GObject *
constructor (GType type, guint n_props, GObjectConstructParam *props)
{
    GObject *object;
    MilterEventloop *eventloop;
    GObjectClass *klass;
    MilterEventloopClass *eventloop_class;
    MilterEventloopPrivate *priv;

    klass = G_OBJECT_CLASS(milter_eventloop_parent_class);
    object = klass->constructor(type, n_props, props);

    priv = MILTER_EVENTLOOP_GET_PRIVATE(object);

    eventloop = MILTER_EVENTLOOP(object);
    eventloop_class = MILTER_EVENTLOOP_GET_CLASS(object);

    priv->dummy = 0;

    return object;
}

static void
milter_eventloop_init (MilterEventloop *eventloop)
{
    MilterEventloopPrivate *priv;

    priv = MILTER_EVENTLOOP_GET_PRIVATE(eventloop);
    priv->dummy = -1;
}

static void
dispose (GObject *object)
{
    MilterEventloopPrivate *priv;

    priv = MILTER_EVENTLOOP_GET_PRIVATE(object);

    G_OBJECT_CLASS(milter_eventloop_parent_class)->dispose(object);
}

static void
set_property (GObject      *object,
              guint         prop_id,
              const GValue *value,
              GParamSpec   *pspec)
{
    MilterEventloopPrivate *priv;

    priv = MILTER_EVENTLOOP_GET_PRIVATE(object);
    switch (prop_id) {
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

static void
get_property (GObject    *object,
              guint       prop_id,
              GValue     *value,
              GParamSpec *pspec)
{
    MilterEventloop *eventloop;
    MilterEventloopPrivate *priv;

    eventloop = MILTER_EVENTLOOP(object);
    priv = MILTER_EVENTLOOP_GET_PRIVATE(eventloop);
    switch (prop_id) {
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

GQuark
milter_eventloop_error_quark (void)
{
    return g_quark_from_static_string("milter-eventloop-error-quark");
}

/*
vi:ts=4:nowrap:ai:expandtab:sw=4
*/
