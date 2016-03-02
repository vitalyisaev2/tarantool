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
#include "tuple.h"
#include "txn.h"
#include "index.h"
#include "bit/bit.h"
#include "space.h"
#include "iproto_constants.h"
#include "request.h"
#include "sophia_index.h"
#include "sophia_space.h"
#include "sophia_engine.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <sophia.h>

struct tuple *
SophiaSpace::executeReplace(struct txn*, struct space *space,
                            struct request *request)
{
	SophiaEngine *engine = (SophiaEngine *)space->handler->engine;
	SophiaIndex *index = (SophiaIndex *)index_find(space, 0);

	/* check field count in tuple */
	space_validate_tuple_raw(space, request->tuple);

	int size = request->tuple_end - request->tuple;
	const char *key =
		tuple_field_raw(request->tuple, size,
		                index->key_def->parts[0].fieldno);
	primary_key_validate(index->key_def, key, index->key_def->part_count);

	/* switch from INSERT to REPLACE during recovery */
	if (request->type == IPROTO_INSERT && engine->recovery_complete) {
		/* ensure key does not exists */
		struct tuple *found = index->findByKey(key, 0);
		if (found) {
			tuple_delete(found);
			tnt_raise(ClientError, ER_TUPLE_FOUND,
			          index_name(index), space_name(space));
		}
	}

	/* replace */
	void *obj = index->create_document(index, &key);
	if (obj == NULL)
		sophia_error(engine->env);
	const char *value = key;
	size_t valuesize;
	valuesize = size - (value - request->tuple);
	if (valuesize > 0)
		sp_setstring(obj, "value", value, valuesize);
	int rc;
	rc = sp_set(in_txn()->engine_tx, obj);
	if (rc == -1)
		sophia_error(engine->env);
	return NULL;
}

struct tuple *
SophiaSpace::executeDelete(struct txn*, struct space *space,
                           struct request *request)
{
	SophiaEngine *engine = (SophiaEngine *)space->handler->engine;
	SophiaIndex *index = (SophiaIndex *)index_find(space, request->index_id);
	const char *key = request->key;
	uint32_t part_count = mp_decode_array(&key);
	primary_key_validate(index->key_def, key, part_count);

	void *obj = index->create_document(index, &key);
	if (obj == NULL)
		sophia_error(engine->env);
	int rc = sp_delete(in_txn()->engine_tx, obj);
	if (rc == -1)
		sophia_error(engine->env);
	return NULL;
}

struct tuple *
SophiaSpace::executeUpdate(struct txn*, struct space *space,
                           struct request *request)
{
	/* try to find the tuple by unique key */
	SophiaEngine *engine = (SophiaEngine *)space->handler->engine;
	SophiaIndex *index = (SophiaIndex *)index_find(space, request->index_id);
	const char *key = request->key;
	uint32_t part_count = mp_decode_array(&key);
	primary_key_validate(index->key_def, key, part_count);
	struct tuple *old_tuple = index->findByKey(key, part_count);

	if (old_tuple == NULL)
		return NULL;

	/* sophia always yields a zero-ref tuple, gc it here */
	TupleRef old_ref(old_tuple);

	/* do tuple update */
	struct tuple *new_tuple =
		tuple_update(space->format,
		             region_aligned_alloc_xc_cb,
		             &fiber()->gc,
		             old_tuple, request->tuple,
		             request->tuple_end,
		             request->index_base);
	TupleRef ref(new_tuple);

	space_validate_tuple(space, new_tuple);
	space_check_update(space, old_tuple, new_tuple);

	key = tuple_field_raw(new_tuple->data,
	                      new_tuple->bsize,
	                      index->key_def->parts[0].fieldno);

	/* replace */
	void *obj = index->create_document(index, &key);
	if (obj == NULL)
		sophia_error(engine->env);
	const char *value = key;
	size_t valuesize;
	valuesize = new_tuple->bsize - (value - new_tuple->data);
	if (valuesize > 0)
		sp_setstring(obj, "value", value, valuesize);
	int rc;
	rc = sp_set(in_txn()->engine_tx, obj);
	if (rc == -1)
		sophia_error(engine->env);
	return NULL;
}

void
SophiaSpace::executeUpsert(struct txn*, struct space *space,
                           struct request *request)
{
	SophiaEngine *engine = (SophiaEngine *)space->handler->engine;
	SophiaIndex *index = (SophiaIndex *)index_find(space, request->index_id);

	/* check field count in tuple */
	space_validate_tuple_raw(space, request->tuple);

	/* check tuple fields */
	tuple_validate_raw(space->format, request->tuple);

	const char *ptr;
	const char *expr = request->ops;
	const char *expr_end = request->ops_end;
	const char *tuple = request->tuple;
	const char *tuple_end = request->tuple_end;
	uint8_t index_base = request->index_base;

	/* upsert */
	mp_decode_array(&tuple);
	uint32_t expr_size  = expr_end - expr;
	uint32_t tuple_size = tuple_end - tuple;
	ptr = tuple;
	void *obj = index->create_document(index, &ptr);
	const char *tuple_value = ptr;
	uint32_t tuple_value_size;
	tuple_value_size = tuple_size - (tuple_value - tuple);
	uint32_t value_size =
		sizeof(uint8_t) + sizeof(uint32_t) + tuple_value_size + expr_size;
	char *value = (char *)malloc(value_size);
	if (value == NULL) {
	}
	char *p = value;
	memcpy(p, &index_base, sizeof(uint8_t));
	p += sizeof(uint8_t);
	memcpy(p, &tuple_value_size, sizeof(uint32_t));
	p += sizeof(uint32_t);
	memcpy(p, tuple_value, tuple_value_size);
	p += tuple_value_size;
	memcpy(p, expr, expr_size);
	sp_setstring(obj, "value", value, value_size);
	void *transaction = in_txn()->engine_tx;
	int rc = sp_upsert(transaction, obj);
	free(value);
	if (rc == -1)
		sophia_error(engine->env);
}

SophiaSpace::SophiaSpace(Engine *e)
	:Handler(e)
{ }

struct sophia_mempool {
	void *chunks[128];
	int count;
};

static inline void
sophia_mempool_init(sophia_mempool *p)
{
	memset(p->chunks, 0, sizeof(p->chunks));
	p->count = 0;
}

static inline void
sophia_mempool_free(sophia_mempool *p)
{
	int i = 0;
	while (i < p->count) {
		free(p->chunks[i]);
		i++;
	}
}

static void *
sophia_upsert_alloc(void *arg, size_t size)
{
	/* simulate region allocator for use with
	 * tuple_upsert_execute() */
	struct sophia_mempool *p = (struct sophia_mempool*)arg;
	assert(p->count < 128);
	void *ptr = malloc(size);
	p->chunks[p->count++] = ptr;
	return ptr;
}

static inline int
sophia_upsert_mp(char **tuple, int *tuple_size_key, struct key_def *key_def,
                 char **key, int *key_size,
                 char *src, int src_size)
{
	/* calculate msgpack size */
	uint32_t mp_keysize = 0;
	uint32_t i = 0;
	while (i < key_def->part_count) {
		if (key_def->parts[i].type == STRING)
			mp_keysize += mp_sizeof_str(key_size[i]);
		else
			mp_keysize += mp_sizeof_uint(load_u64(key[i]));
		i++;
	}
	*tuple_size_key = mp_keysize + mp_sizeof_array(key_def->part_count);

	/* count fields */
	int count = key_def->part_count;
	const char *p = src;
	while (p < (src + src_size)) {
		count++;
		mp_next((const char **)&p);
	}

	/* allocate and encode tuple */
	int mp_size = mp_sizeof_array(count) +
		mp_keysize + src_size;
	char *mp = (char *)malloc(mp_size);
	char *mp_ptr = mp;
	if (mp == NULL)
		return -1;
	mp_ptr = mp_encode_array(mp_ptr, count);
	i = 0;
	while (i < key_def->part_count) {
		if (key_def->parts[i].type == STRING)
			mp_ptr = mp_encode_str(mp_ptr, key[i], key_size[i]);
		else
			mp_ptr = mp_encode_uint(mp_ptr, load_u64(key[i]));
		i++;
	}
	memcpy(mp_ptr, src, src_size);

	*tuple = mp;
	return mp_size;
}

static inline int
sophia_upsert(char **result,
              char *tuple, int tuple_size, int tuple_size_key,
              char *upsert, int upsert_size)
{
	char *p = upsert;
	uint8_t index_base = *(uint8_t *)p;
	p += sizeof(uint8_t);
	uint32_t default_tuple_size = *(uint32_t *)p;
	p += sizeof(uint32_t);
	p += default_tuple_size;
	char *expr = p;
	char *expr_end = upsert + upsert_size;
	const char *up;
	uint32_t up_size;
	/* emit upsert */
	struct sophia_mempool alloc;
	sophia_mempool_init(&alloc);
	try {
		up = tuple_upsert_execute(sophia_upsert_alloc, &alloc,
		                          expr,
		                          expr_end,
		                          tuple,
		                          tuple + tuple_size,
		                          &up_size, index_base);
	} catch (Exception *e) {
		sophia_mempool_free(&alloc);
		return -1;
	}
	/* get new value */
	int size = up_size - tuple_size_key;
	*result = (char *)malloc(size);
	if (! *result) {
		sophia_mempool_free(&alloc);
		return -1;
	}
	memcpy(*result, up + tuple_size_key, size);
	sophia_mempool_free(&alloc);
	return size;
}

int
sophia_upsert_callback(char **result,
                       char **key, int *key_size, int /* key_count */,
                       char *src, int src_size,
                       char *upsert, int upsert_size,
                       void *arg)
{
	/* use default tuple value */
	if (src == NULL) {
		char *p = upsert;
		p += sizeof(uint8_t); /* index base */
		uint32_t value_size = *(uint32_t *)p;
		p += sizeof(uint32_t);
		*result = (char *)malloc(value_size);
		if (! *result)
			return -1;
		memcpy(*result, p, value_size);
		return value_size;
	}
	struct key_def *key_def = (struct key_def *)arg;
	/* convert to msgpack */
	char *tuple;
	int tuple_size_key;
	int tuple_size;
	tuple_size = sophia_upsert_mp(&tuple, &tuple_size_key,
	                              key_def, key, key_size,
	                              src, src_size);
	if (tuple_size == -1)
		return -1;
	/* execute upsert */
	int size;
	size = sophia_upsert(result,
	                     tuple, tuple_size, tuple_size_key,
	                     upsert,
	                     upsert_size);
	free(tuple);
	return size;
}
