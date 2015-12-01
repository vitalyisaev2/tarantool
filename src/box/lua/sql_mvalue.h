#ifndef SQL_MVALUE_H
#define SQL_MVALUE_H

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

 #include "box/index.h"

class MValue {
private:
	int type;
	void *data;
	int data_len;
	mutable bool error;

static int do_mvalue_from_msgpuck(MValue *res, const char **data);

public:
	MValue();
	MValue(const MValue &ob);

	bool IsError() const;
	bool IsEmpty() const;
	int GetType() const;

	uint64_t GetUint64() const;
	int64_t GetInt64() const;
	const char *GetStr(size_t *len = NULL) const;
	const char *GetBin(size_t *len = NULL) const;
	bool GetBool() const;
	MValue *operator[](int index);
	const MValue *operator[](int index) const;
	MValue &operator=(const MValue &ob);

	static MValue FromMSGPuck(const char **data);

	void Clear();

	~MValue();
};

// uint32_t get_hash_mvalue(mvalue *);

// #if !MH_SOURCE
// #define MH_UNDEF
// #endif

// #include "third_party/PMurHash.h"

// /*
//  * Map: (mvalue *) => (mvalue *)
//  */
// enum {
// 	MH_MVALUE_HASH_SEED = 19U
// };

// #define mh_name _mvalptr
// struct mh_mvalptr_key_t {
// 	mvalue *key;
// 	uint32_t hash;
// };
// #define mh_key_t struct mh_mvalptr_key_t *

// struct mh_mvalptr_node_t {
// 	mvalue *key;
// 	uint32_t hash;
// 	mvalue *val;
// };
// #define mh_node_t struct mh_mvalptr_node_t

// #define mh_arg_t mvalue *
// #define mh_hash(a, arg) ((a)->hash)
// #define mh_hash_key(a, arg) ((a)->hash)
// #define mh_cmp(a, b, arg) ((a)->len != (b)->len || \
// 			    strncmp((a)->str, (b)->str, (a)->len))
// #define mh_cmp_key(a, b, arg) mh_cmp(a, b, arg)
// #include "salad/mhash.h"

#endif