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
#include "net_box.h"

#include <small/ibuf.h>
#include <msgpuck.h> /* mp_store_u32() */
#include "scramble.h"

#include "box/iproto_constants.h"
#include "box/lua/tuple.h" /* luamp_convert_tuple() / luamp_convert_key() */

#include "lua/msgpack.h"
#include "third_party/base64.h"

#define cfg luaL_msgpack_default

static inline size_t
netbox_prepare_request(lua_State *L, struct mpstream *stream, uint32_t r_type)
{
	struct ibuf *ibuf = (struct ibuf *) lua_topointer(L, 1);
	uint64_t sync = luaL_touint64(L, 2);
	uint64_t schema_id = luaL_touint64(L, 3);

	mpstream_init(stream, ibuf, ibuf_reserve_cb, ibuf_alloc_cb,
		      luamp_error, L);

	/* Remember initial size of ibuf (see netbox_encode_request()) */
	size_t used = ibuf_used(ibuf);

	/* Reserve and skip space for fixheader */
	size_t fixheader_size = mp_sizeof_uint(UINT32_MAX);
	mpstream_reserve(stream, fixheader_size);
	mpstream_advance(stream, fixheader_size);

	/* encode header */
	mpstream_encode_map(stream, 3);

	mpstream_encode_uint(stream, IPROTO_SYNC);
	mpstream_encode_uint(stream, sync);

	mpstream_encode_uint(stream, IPROTO_SCHEMA_ID);
	mpstream_encode_uint(stream, schema_id);

	mpstream_encode_uint(stream, IPROTO_REQUEST_TYPE);
	mpstream_encode_uint(stream, r_type);

	/* Caller should remember how many bytes was used in ibuf */
	return used;
}

static inline void
netbox_encode_request(struct mpstream *stream, size_t initial_size)
{
	mpstream_flush(stream);

	struct ibuf *ibuf = (struct ibuf *) stream->ctx;

	/*
	 * Calculation the start position in ibuf by getting current size
	 * and then substracting initial size. Since we don't touch
	 * ibuf->rpos during encoding this approach should always work
	 * even on realloc or memmove inside ibuf.
	 */
	size_t fixheader_size = mp_sizeof_uint(UINT32_MAX);
	size_t used = ibuf_used(ibuf);
	assert(initial_size + fixheader_size <= used);
	size_t total_size = used - initial_size;
	char *fixheader = ibuf->wpos - total_size;
	assert(fixheader >= ibuf->rpos);

	/* patch skipped len */
	*(fixheader++) = 0xce;
	/* fixheader size is not included */
	mp_store_u32(fixheader, total_size - fixheader_size);
}

static int
netbox_encode_ping(lua_State *L)
{
	if (lua_gettop(L) < 3)
		return luaL_error(L, "Usage: netbox.encode_ping(ibuf, sync, "
				"schema_id)");

	struct mpstream stream;
	size_t svp = netbox_prepare_request(L, &stream, IPROTO_PING);
	netbox_encode_request(&stream, svp);
	return 0;
}

static int
netbox_encode_auth(lua_State *L)
{
	if (lua_gettop(L) < 6)
		return luaL_error(L, "Usage: netbox.encode_update(ibuf, sync, "
		       "schema_id, user, password, greeting)");

	struct mpstream stream;
	size_t svp = netbox_prepare_request(L, &stream, IPROTO_AUTH);

	size_t user_len;
	const char *user = lua_tolstring(L, 4, &user_len);
	size_t password_len;
	const char *password = lua_tolstring(L, 5, &password_len);
	size_t salt_len;
	const char *salt = lua_tolstring(L, 6, &salt_len);
	if (salt_len < SCRAMBLE_SIZE)
		return luaL_error(L, "Invalid salt");

	/* Adapted from xrow_encode_auth() */
	mpstream_encode_map(&stream, password != NULL ? 2 : 1);
	mpstream_encode_uint(&stream, IPROTO_USER_NAME);
	mpstream_encode_str(&stream, user, user_len);
	if (password != NULL) { /* password can be omitted */
		char scramble[SCRAMBLE_SIZE];
		scramble_prepare(scramble, salt, password, password_len);
		mpstream_encode_uint(&stream, IPROTO_TUPLE);
		mpstream_encode_array(&stream, 2);
		mpstream_encode_str(&stream, "chap-sha1", strlen("chap-sha1"));
		mpstream_encode_str(&stream, scramble, SCRAMBLE_SIZE);
	}

	netbox_encode_request(&stream, svp);
	return 0;
}

static int
netbox_encode_call(lua_State *L)
{
	if (lua_gettop(L) < 5)
		return luaL_error(L, "Usage: netbox.encode_call(ibuf, sync, "
		       "schema_id, function_name, args)");

	struct mpstream stream;
	size_t svp = netbox_prepare_request(L, &stream, IPROTO_CALL);

	mpstream_encode_map(&stream, 2);

	/* encode proc name */
	size_t name_len;
	const char *name = lua_tolstring(L, 4, &name_len);
	mpstream_encode_uint(&stream, IPROTO_FUNCTION_NAME);
	mpstream_encode_str(&stream, name, name_len);

	/* encode args */
	mpstream_encode_uint(&stream, IPROTO_TUPLE);
	luamp_encode_tuple(L, cfg, &stream, 5);

	netbox_encode_request(&stream, svp);
	return 0;
}

static int
netbox_encode_eval(lua_State *L)
{
	if (lua_gettop(L) < 5)
		return luaL_error(L, "Usage: netbox.encode_eval(ibuf, sync, "
		       "schema_id, expr, args)");

	struct mpstream stream;
	size_t svp = netbox_prepare_request(L, &stream, IPROTO_EVAL);

	mpstream_encode_map(&stream, 2);

	/* encode expr */
	size_t expr_len;
	const char *expr = lua_tolstring(L, 4, &expr_len);
	mpstream_encode_uint(&stream, IPROTO_EXPR);
	mpstream_encode_str(&stream, expr, expr_len);

	/* encode args */
	mpstream_encode_uint(&stream, IPROTO_TUPLE);
	luamp_encode_tuple(L, cfg, &stream, 5);

	netbox_encode_request(&stream, svp);
	return 0;
}

static int
netbox_encode_select(lua_State *L)
{
	if (lua_gettop(L) < 9)
		return luaL_error(L, "Usage netbox.encode_select(ibuf, sync, "
				  "schema_id, space_id, index_id, iterator, "
				  "offset, limit, key)");

	struct mpstream stream;
	size_t svp = netbox_prepare_request(L, &stream, IPROTO_SELECT);

	mpstream_encode_map(&stream, 6);

	uint32_t space_id = lua_tointeger(L, 4);
	uint32_t index_id = lua_tointeger(L, 5);
	int iterator = lua_tointeger(L, 6);
	uint32_t offset = lua_tointeger(L, 7);
	uint32_t limit = lua_tointeger(L, 8);

	/* encode space_id */
	mpstream_encode_uint(&stream, IPROTO_SPACE_ID);
	mpstream_encode_uint(&stream, space_id);

	/* encode index_id */
	mpstream_encode_uint(&stream, IPROTO_INDEX_ID);
	mpstream_encode_uint(&stream, index_id);

	/* encode iterator */
	mpstream_encode_uint(&stream, IPROTO_ITERATOR);
	mpstream_encode_uint(&stream, iterator);

	/* encode offset */
	mpstream_encode_uint(&stream, IPROTO_OFFSET);
	mpstream_encode_uint(&stream, offset);

	/* encode limit */
	mpstream_encode_uint(&stream, IPROTO_LIMIT);
	mpstream_encode_uint(&stream, limit);

	/* encode key */
	mpstream_encode_uint(&stream, IPROTO_KEY);
	luamp_convert_key(L, cfg, &stream, 9);

	netbox_encode_request(&stream, svp);
	return 0;
}

static inline int
netbox_encode_insert_or_replace(lua_State *L, uint32_t reqtype)
{
	if (lua_gettop(L) < 5)
		return luaL_error(L, "Usage: netbox.encode_insert(ibuf, sync, "
		       "schema_id, space_id, tuple)");
	lua_settop(L, 5);

	struct mpstream stream;
	size_t svp = netbox_prepare_request(L, &stream, reqtype);

	mpstream_encode_map(&stream, 2);

	/* encode space_id */
	uint32_t space_id = lua_tointeger(L, 4);
	mpstream_encode_uint(&stream, IPROTO_SPACE_ID);
	mpstream_encode_uint(&stream, space_id);

	/* encode args */
	mpstream_encode_uint(&stream, IPROTO_TUPLE);
	luamp_encode_tuple(L, cfg, &stream, 5);

	netbox_encode_request(&stream, svp);
	return 0;
}

static int
netbox_encode_insert(lua_State *L)
{
	return netbox_encode_insert_or_replace(L, IPROTO_INSERT);
}

static int
netbox_encode_replace(lua_State *L)
{
	return netbox_encode_insert_or_replace(L, IPROTO_REPLACE);
}

static int
netbox_encode_delete(lua_State *L)
{
	if (lua_gettop(L) < 6)
		return luaL_error(L, "Usage: netbox.encode_delete(ibuf, sync, "
		       "schema_id, space_id, index_id, key)");

	struct mpstream stream;
	size_t svp = netbox_prepare_request(L, &stream, IPROTO_DELETE);

	mpstream_encode_map(&stream, 3);

	/* encode space_id */
	uint32_t space_id = lua_tointeger(L, 4);
	mpstream_encode_uint(&stream, IPROTO_SPACE_ID);
	mpstream_encode_uint(&stream, space_id);

	/* encode space_id */
	uint32_t index_id = lua_tointeger(L, 5);
	mpstream_encode_uint(&stream, IPROTO_INDEX_ID);
	mpstream_encode_uint(&stream, index_id);

	/* encode key */
	mpstream_encode_uint(&stream, IPROTO_KEY);
	luamp_convert_key(L, cfg, &stream, 6);

	netbox_encode_request(&stream, svp);
	return 0;
}

static int
netbox_encode_update(lua_State *L)
{
	if (lua_gettop(L) < 7)
		return luaL_error(L, "Usage: netbox.encode_update(ibuf, sync, "
		       "schema_id, space_id, index_id, key, ops)");

	struct mpstream stream;
	size_t svp = netbox_prepare_request(L, &stream, IPROTO_UPDATE);

	mpstream_encode_map(&stream, 5);

	/* encode space_id */
	uint32_t space_id = lua_tointeger(L, 4);
	mpstream_encode_uint(&stream, IPROTO_SPACE_ID);
	mpstream_encode_uint(&stream, space_id);

	/* encode index_id */
	uint32_t index_id = lua_tointeger(L, 5);
	mpstream_encode_uint(&stream, IPROTO_INDEX_ID);
	mpstream_encode_uint(&stream, index_id);

	/* encode index_id */
	mpstream_encode_uint(&stream, IPROTO_INDEX_BASE);
	mpstream_encode_uint(&stream, 1);

	/* encode in reverse order for speedup - see luamp_encode() code */
	/* encode ops */
	mpstream_encode_uint(&stream, IPROTO_TUPLE);
	luamp_encode_tuple(L, cfg, &stream, 7);
	lua_pop(L, 1); /* ops */

	/* encode key */
	mpstream_encode_uint(&stream, IPROTO_KEY);
	luamp_convert_key(L, cfg, &stream, 6);

	netbox_encode_request(&stream, svp);
	return 0;
}

static int
netbox_encode_upsert(lua_State *L)
{
	if (lua_gettop(L) != 7)
		return luaL_error(L, "Usage: netbox.encode_update(ibuf, sync, "
			"schema_id, space_id, index_id, tuple, ops)");

	struct mpstream stream;
	size_t svp = netbox_prepare_request(L, &stream, IPROTO_UPSERT);

	mpstream_encode_map(&stream, 6);

	/* encode space_id */
	uint32_t space_id = lua_tointeger(L, 4);
	mpstream_encode_uint(&stream, IPROTO_SPACE_ID);
	mpstream_encode_uint(&stream, space_id);

	/* encode index_id */
	uint32_t index_id = lua_tointeger(L, 5);
	mpstream_encode_uint(&stream, IPROTO_INDEX_ID);
	mpstream_encode_uint(&stream, index_id);

	/* encode index_id */
	mpstream_encode_uint(&stream, IPROTO_INDEX_BASE);
	mpstream_encode_uint(&stream, 1);

	/* encode in reverse order for speedup - see luamp_encode() code */
	/* encode ops */
	mpstream_encode_uint(&stream, IPROTO_OPS);
	luamp_encode_tuple(L, cfg, &stream, 7);
	lua_pop(L, 1); /* ops */

	/* encode tuple */
	mpstream_encode_uint(&stream, IPROTO_TUPLE);
	luamp_encode_tuple(L, cfg, &stream, 6);

	netbox_encode_request(&stream, svp);
	return 0;
}

int
luaopen_net_box(struct lua_State *L)
{
	const luaL_reg net_box_lib[] = {
		{ "encode_ping",    netbox_encode_ping },
		{ "encode_call",    netbox_encode_call },
		{ "encode_eval",    netbox_encode_eval },
		{ "encode_select",  netbox_encode_select },
		{ "encode_insert",  netbox_encode_insert },
		{ "encode_replace", netbox_encode_replace },
		{ "encode_delete",  netbox_encode_delete },
		{ "encode_update",  netbox_encode_update },
		{ "encode_upsert",  netbox_encode_upsert },
		{ "encode_auth",    netbox_encode_auth },
		{ NULL, NULL}
	};
	luaL_register(L, "net.box.lib", net_box_lib);
	return 1;
}
