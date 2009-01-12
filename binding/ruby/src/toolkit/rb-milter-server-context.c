/* -*- c-file-style: "ruby" -*- */
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
 *  along with this library.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "rb-milter-server-private.h"

#define SELF(self) (MILTER_SERVER_CONTEXT(RVAL2GOBJ(self)))

static VALUE
set_connection_spec (VALUE self, VALUE spec)
{
    GError *error = NULL;

    if (!milter_server_context_set_connection_spec(SELF(self),
						   RVAL2CSTR(spec),
						   &error))
	RAISE_GERROR(error);

    return self;
}

static VALUE
establish_connection (VALUE self)
{
    GError *error = NULL;

    if (!milter_server_context_establish_connection(SELF(self), &error))
	RAISE_GERROR(error);

    return Qnil;
}

static VALUE
context_connect (VALUE self, VALUE host, VALUE address)
{
    VALUE rb_packed_address;
    struct sockaddr *packed_address;
    socklen_t packed_address_length;
    gboolean success;

    if (RVAL2CBOOL(rb_obj_is_kind_of(address, rb_cString)))
	rb_packed_address = address;
    else
	rb_packed_address = rb_funcall(address, rb_intern("pack"), 0);

    packed_address = (struct sockaddr *)(RSTRING_PTR(rb_packed_address));
    packed_address_length = RSTRING_LEN(rb_packed_address);
    success = milter_server_context_connect(SELF(self),
					    RVAL2CSTR(host),
					    packed_address,
					    packed_address_length);

    return CBOOL2RVAL(success);
}

static VALUE
context_helo (VALUE self, VALUE fqdn)
{
    gboolean success;

    success = milter_server_context_helo(SELF(self), RVAL2CSTR(fqdn));

    return CBOOL2RVAL(success);
}

static VALUE
context_envelope_from (VALUE self, VALUE envelope_from)
{
    gboolean success;

    success = milter_server_context_envelope_from(SELF(self),
						  RVAL2CSTR(envelope_from));

    return CBOOL2RVAL(success);
}

static VALUE
context_envelope_recipient (VALUE self, VALUE envelope_recipient)
{
    gboolean success;

    success = milter_server_context_envelope_recipient(
	SELF(self), RVAL2CSTR(envelope_recipient));

    return CBOOL2RVAL(success);
}

static VALUE
context_header (VALUE self, VALUE name, VALUE value)
{
    gboolean success;

    success = milter_server_context_header(SELF(self),
					   RVAL2CSTR(name), RVAL2CSTR(value));

    return CBOOL2RVAL(success);
}

static VALUE
context_end_of_header (VALUE self)
{
    gboolean success;

    success = milter_server_context_end_of_header(SELF(self));

    return CBOOL2RVAL(success);
}

static VALUE
context_body (VALUE self, VALUE chunk)
{
    gboolean success;

    success = milter_server_context_body(SELF(self),
					 RSTRING_PTR(chunk), RSTRING_LEN(chunk));

    return CBOOL2RVAL(success);
}

static VALUE
context_end_of_message (int argc, VALUE *argv, VALUE self)
{
    gboolean success;
    VALUE rb_chunk;
    gchar *chunk = NULL;
    gsize size = 0;

    rb_scan_args(argc, argv, "01", &rb_chunk);

    if (!NIL_P(rb_chunk)) {
	chunk = RSTRING_PTR(rb_chunk);
	size = RSTRING_LEN(rb_chunk);
    }
    success = milter_server_context_end_of_message(SELF(self), chunk, size);

    return CBOOL2RVAL(success);
}

static VALUE
stop_body_signal_convert (guint num, const GValue *values)
{
    return rb_ary_new3(2,
		       GVAL2RVAL(&values[0]),
		       rb_str_new(g_value_get_string(&values[1]),
#if GLIB_SIZEOF_SIZE_T == 8
				  g_value_get_uint64(&values[2])
#else
				  g_value_get_uint(&values[2])
#endif
			   ));
}

static VALUE
stop_end_of_message_signal_convert (guint num, const GValue *values)
{
    VALUE rb_chunk = Qnil;
    const gchar *chunk;
    gsize size;

    chunk = g_value_get_string(&values[1]);
#if GLIB_SIZEOF_SIZE_T == 8
    size = g_value_get_uint64(&values[2]);
#else
    size = g_value_get_uint(&values[2]);
#endif

    if (chunk && size > 0)
	rb_chunk = rb_str_new(chunk, size);

    return rb_ary_new3(2, GVAL2RVAL(&values[0]), rb_chunk);
}

void
Init_milter_server_context (void)
{
    VALUE rb_cMilterServerContext;

    rb_cMilterServerContext = G_DEF_CLASS(MILTER_TYPE_SERVER_CONTEXT,
                                          "ServerContext", rb_mMilter);
    G_DEF_ERROR2(MILTER_SERVER_CONTEXT_ERROR,
		 "ServerContextError", rb_mMilter, rb_eMilterError);

    rb_define_method(rb_cMilterServerContext, "set_connection_spec",
		     set_connection_spec, 1);
    rb_define_method(rb_cMilterServerContext, "establish_connection",
		     establish_connection, 0);

    rb_define_method(rb_cMilterServerContext, "connect", context_connect, 2);
    rb_define_method(rb_cMilterServerContext, "helo", context_helo, 1);
    rb_define_method(rb_cMilterServerContext, "envelope_from",
		     context_envelope_from, 1);
    rb_define_method(rb_cMilterServerContext, "envelope_recipient",
		     context_envelope_recipient, 1);
    rb_define_method(rb_cMilterServerContext, "header", context_header, 2);
    rb_define_method(rb_cMilterServerContext, "end_of_header",
		     context_end_of_header, 0);
    rb_define_method(rb_cMilterServerContext, "body", context_body, 1);
    rb_define_method(rb_cMilterServerContext, "end_of_message",
		     context_end_of_message, -1);

    G_DEF_SIGNAL_FUNC(rb_cMilterServerContext, "stop-on-connect",
		      rb_milter__connect_signal_convert);
    G_DEF_SIGNAL_FUNC(rb_cMilterServerContext, "stop-on-body",
		      stop_body_signal_convert);
    G_DEF_SIGNAL_FUNC(rb_cMilterServerContext, "stop-on-end-of-message",
		      stop_end_of_message_signal_convert);

    G_DEF_SETTERS(rb_cMilterServerContext);
}
