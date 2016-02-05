/*
 * Copyright 2010-2015, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "lua/msgpack.h"

#include "lua/utils.h"

#if defined(LUAJIT)
#include <lj_ctype.h>
#endif /* defined(LUAJIT) */
#include <lauxlib.h> /* struct luaL_error */

#include <msgpuck.h>
#include <small/region.h>
#include <small/ibuf.h>

#include <iobuf.h>
#include <fiber.h>

#include <box/error.h>
#include <box/errcode.h>

struct luaL_serializer *luaL_msgpack_default = NULL;

static enum mp_type
luamp_encode_extension_default(struct lua_State *L, int idx,
			       struct mpstream *stream);

static void
luamp_decode_extension_default(struct lua_State *L, const char **data);

static luamp_encode_extension_f luamp_encode_extension =
		luamp_encode_extension_default;
static luamp_decode_extension_f luamp_decode_extension =
		luamp_decode_extension_default;

#define box_error_fmt(EC, ...) \
	box_error_raise((EC), tnt_errcode_str((EC)), ##__VA_ARGS__)

static inline void *
mpstream_reserve_diag_cb(void *ctx, size_t *size)
{
	void *ptr = ibuf_reserve_cb(ctx, size);
	if (ptr == NULL)
		box_error_fmt(ER_MEMORY_ISSUE, *size, "mpstream", "reserve");
	return ptr;
}

static inline void *
mpstream_alloc_diag_cb(void *ctx, size_t size)
{
	void *ptr = ibuf_alloc_cb(ctx, size);
	if (ptr == NULL)
		box_error_fmt(ER_MEMORY_ISSUE, size, "mpstream", "alloc");
	return ptr;
}

#undef box_error_fmt

void
luamp_error(void *error_ctx, const char *err, size_t errlen)
{
	(void )errlen;
	struct lua_State *L = (struct lua_State *) error_ctx;
	luaL_error(L, err);
}

void
luamp_set_encode_extension(luamp_encode_extension_f handler)
{
	if (handler == NULL) {
		luamp_encode_extension = luamp_encode_extension_default;
	} else {
		luamp_encode_extension = handler;
	}
}

static void
luamp_decode_extension_default(struct lua_State *L, const char **data)
{
	luaL_error(L, "msgpack.decode: unsupported extension: %u",
		   (unsigned char) **data);
}


void
luamp_set_decode_extension(luamp_decode_extension_f handler)
{
	if (handler == NULL) {
		luamp_decode_extension = luamp_decode_extension_default;
	} else {
		luamp_decode_extension = handler;
	}
}

static enum mp_type
luamp_encode_extension_default(struct lua_State *L, int idx,
			       struct mpstream *stream)
{
	(void) L;
	(void) idx;
	(void) stream;
	return MP_EXT;
}

static enum mp_type
luamp_encode_r(struct lua_State *L, struct luaL_serializer *cfg,
	       struct mpstream *stream, int level)
{
	int index = lua_gettop(L);

	struct luaL_field field;
	/* Detect field type */
	luaL_tofield(L, cfg, index, &field);
	if (field.type == MP_EXT) {
		/* Run trigger if type can't be encoded */
		enum mp_type type = luamp_encode_extension(L, index, stream);
		if (type != MP_EXT)
			return type; /* Value has been packed by the trigger */
		/* Try to convert value to serializable type */
		luaL_convertfield(L, cfg, index, &field);
	}
	switch (field.type) {
	case MP_UINT:
		mpstream_encode_uint(stream, field.ival);
		return MP_UINT;
	case MP_STR:
		mpstream_encode_str(stream, field.sval.data, field.sval.len);
		return MP_STR;
	case MP_BIN:
		mpstream_encode_str(stream, field.sval.data, field.sval.len);
		return MP_BIN;
	case MP_INT:
		mpstream_encode_int(stream, field.ival);
		return MP_INT;
	case MP_FLOAT:
		mpstream_encode_float(stream, field.fval);
		return MP_FLOAT;
	case MP_DOUBLE:
		mpstream_encode_double(stream, field.dval);
		return MP_DOUBLE;
	case MP_BOOL:
		mpstream_encode_bool(stream, field.bval);
		return MP_BOOL;
	case MP_NIL:
		mpstream_encode_nil(stream);
		return MP_NIL;
	case MP_MAP:
		/* Map */
		if (level >= cfg->encode_max_depth) {
			mpstream_encode_nil(stream); /* Limit nested maps */
			return MP_NIL;
		}
		mpstream_encode_map(stream, field.size);
		lua_pushnil(L);  /* first key */
		while (lua_next(L, index) != 0) {
			lua_pushvalue(L, -2);
			luamp_encode_r(L, cfg, stream, level + 1);
			lua_pop(L, 1);
			luamp_encode_r(L, cfg, stream, level + 1);
			lua_pop(L, 1);
		}
		return MP_MAP;
	case MP_ARRAY:
		/* Array */
		if (level >= cfg->encode_max_depth) {
			mpstream_encode_nil(stream); /* Limit nested arrays */
			return MP_NIL;
		}
		mpstream_encode_array(stream, field.size);
		for (uint32_t i = 0; i < field.size; i++) {
			lua_rawgeti(L, index, i + 1);
			luamp_encode_r(L, cfg, stream, level + 1);
			lua_pop(L, 1);
		}
		return MP_ARRAY;
	case MP_EXT:
		/* handled by luaL_convertfield */
		assert(false);
	}
	return MP_EXT;
}

enum mp_type
luamp_encode(struct lua_State *L, struct luaL_serializer *cfg,
	     struct mpstream *stream, int index)
{
	int top = lua_gettop(L);
	if (index < 0)
		index = top + index + 1;

	bool on_top = (index == top);
	if (!on_top) {
		lua_pushvalue(L, index); /* copy a value to the stack top */
	}

	enum mp_type top_type = luamp_encode_r(L, cfg, stream, 0);

	if (!on_top) {
		lua_remove(L, top + 1); /* remove a value copy */
	}

	return top_type;
}

void
luamp_decode(struct lua_State *L, struct luaL_serializer *cfg,
	     const char **data)
{
	double d;
	switch (mp_typeof(**data)) {
	case MP_UINT:
		luaL_pushuint64(L, mp_decode_uint(data));
		break;
	case MP_INT:
		luaL_pushint64(L, mp_decode_int(data));
		break;
	case MP_FLOAT:
		d = mp_decode_float(data);
		luaL_checkfinite(L, cfg, d);
		lua_pushnumber(L, d);
		return;
	case MP_DOUBLE:
		d = mp_decode_double(data);
		luaL_checkfinite(L, cfg, d);
		lua_pushnumber(L, d);
		return;
	case MP_STR:
	{
		uint32_t len = 0;
		const char *str = mp_decode_str(data, &len);
		lua_pushlstring(L, str, len);
		return;
	}
	case MP_BIN:
	{
		uint32_t len = 0;
		const char *str = mp_decode_bin(data, &len);
		lua_pushlstring(L, str, len);
		return;
	}
	case MP_BOOL:
		lua_pushboolean(L, mp_decode_bool(data));
		return;
	case MP_NIL:
		mp_decode_nil(data);
		luaL_pushnull(L);
		return;
	case MP_ARRAY:
	{
		uint32_t size = mp_decode_array(data);
		lua_createtable(L, size, 0);
		for (uint32_t i = 0; i < size; i++) {
			luamp_decode(L, cfg, data);
			lua_rawseti(L, -2, i + 1);
		}
		if (cfg->decode_save_metatables)
			luaL_setarrayhint(L, -1);
		return;
	}
	case MP_MAP:
	{
		uint32_t size = mp_decode_map(data);
		lua_createtable(L, 0, size);
		for (uint32_t i = 0; i < size; i++) {
			luamp_decode(L, cfg, data);
			luamp_decode(L, cfg, data);
			lua_settable(L, -3);
		}
		if (cfg->decode_save_metatables)
			luaL_setmaphint(L, -1);
		return;
	}
	case MP_EXT:
		luamp_decode_extension(L, data);
		break;
	}
}

static int
lua_msgpack_encode(lua_State *L)
{
	int index = lua_gettop(L);
	if (index != 1)
		luaL_error(L, "msgpack.encode: a Lua object expected");

	struct luaL_serializer *cfg = luaL_checkserializer(L);

	struct ibuf *buf = tarantool_lua_ibuf;
	ibuf_reset(buf);

	struct mpstream stream;
	mpstream_init(&stream, buf, mpstream_reserve_diag_cb,
		      mpstream_alloc_diag_cb, luamp_error, L);

	luamp_encode_r(L, cfg, &stream, 0);
	mpstream_flush(&stream);

	lua_pushlstring(L, buf->buf, ibuf_used(buf));
	ibuf_reinit(buf);
	return 1;
}

static int
lua_msgpack_decode(lua_State *L)
{
	int index = lua_gettop(L);
	if (index != 2 && index != 1 && lua_type(L, 1) != LUA_TSTRING)
		return luaL_error(L, "msgpack.decode: a Lua string expected");

	size_t data_len;
	uint32_t offset = index > 1 ? lua_tointeger(L, 2) - 1 : 0;
	const char *data = lua_tolstring(L, 1, &data_len);
	if (offset >= data_len)
		luaL_error(L, "msgpack.decode: offset is out of bounds");
	const char *end = data + data_len;

	const char *b = data + offset;
	if (mp_check(&b, end))
		return luaL_error(L, "msgpack.decode: invalid MsgPack");

	struct luaL_serializer *cfg = luaL_checkserializer(L);

	b = data + offset;
	luamp_decode(L, cfg, &b);
	lua_pushinteger(L, b - data + 1);
	return 2;
}

static int
lua_ibuf_msgpack_decode(lua_State *L)
{
	const char **start = (const char **)lua_topointer(L, 1);
	struct luaL_serializer *cfg = luaL_checkserializer(L);
	luamp_decode(L, cfg, start);
	return 2;
}

static int
lua_msgpack_new(lua_State *L);

const luaL_reg msgpacklib[] = {
	{ "encode", lua_msgpack_encode },
	{ "decode", lua_msgpack_decode },
	{ "ibuf_decode", lua_ibuf_msgpack_decode },
	{ "new",	lua_msgpack_new },
	{ NULL, NULL}
};

static int
lua_msgpack_new(lua_State *L)
{
	luaL_newserializer(L, NULL, msgpacklib);
	return 1;
}

LUALIB_API int
luaopen_msgpack(lua_State *L)
{
	luaL_msgpack_default = luaL_newserializer(L, "msgpack", msgpacklib);
	return 1;
}
