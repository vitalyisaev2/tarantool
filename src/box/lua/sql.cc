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
extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include "hash.h"
#include "sql.h"
#include <stdlib.h>
#include <string.h>
#include "sqlite3.h"
#include "sqliteInt.h"
#include "btreeInt.h"
}

#undef likely
#undef unlikely

#include "say.h"
#include "box/index.h"
#include "box/schema.h"
#include "box/txn.h"
#include "box/tuple.h"
#include "salad/rlist.h"
#include "sql_mvalue.h"

#include "lua/utils.h"
#include <string>
#include "sql_tarantool_cursor.h"

static const char *sqlitelib_name = "sqlite";

struct TrntlCursor {
	BtCursor *brother; /* BtCursor for TarantoolCursor */
	TarantoolCursor cursor;
	char *key;
};

typedef struct sql_trntl_self {
	TrntlCursor **cursors;
	int cnt_cursors;
	SIndex **indices;
	int cnt_indices;
} sql_trntl_self;

extern "C" {
sqlite3 *global_db = NULL;

sqlite3 *get_global_db() { return global_db; }

void set_global_db(sqlite3 *db) { global_db = db; }

void sql_tarantool_api_init(sql_tarantool_api *ob);

Hash get_trntl_spaces(void *self_, sqlite3 *db, char **pzErrMsg, Schema *pSchema, Hash *idxHash_);

int trntl_cursor_create(void *self_, Btree *p, int iTable, int wrFlag,
                              struct KeyInfo *pKeyInfo, BtCursor *pCur);

int trntl_cursor_first(void *self_, BtCursor *pCur, int *pRes);

int trntl_cursor_data_size(void *self_, BtCursor *pCur, u32 *pSize);

const void *trntl_cursor_data_fetch(void *self_, BtCursor *pCur, u32 *pAmt);

int trntl_cursor_key_size(void *self, BtCursor *pCur, i64 *pSize);
 
const void *trntl_cursor_key_fetch(void *self, BtCursor *pCur, u32 *pAmt);

int trntl_cursor_next(void *self, BtCursor *pCur, int *pRes);

int trntl_cursor_close(void *self, BtCursor *pCur);

char check_num_on_tarantool_id(void *self, u32 num);

int trntl_cursor_move_to_unpacked(void *self, BtCursor *pCur, UnpackedRecord *pIdxKey, i64 intKey, int biasRight, int *pRes, RecordCompare xRecordCompare);

Table *get_trntl_table_from_tuple(box_tuple_t *tpl, sqlite3 *db, Schema *pSchema);

SIndex *get_trntl_index_from_tuple(box_tuple_t *index_tpl, sqlite3 *db, Table *table, bool &ok);

u32 make_index_id(u32 space_id, u32 index_number) {
	u32 res = 0;
	u32 first = 1 << 30;
	u32 second = (index_number << 28) >> 2;
	u32 third = (space_id << 15) >> 6;
	res = first | second | third;
	return res;
}

u32 make_space_id(u32 space_id) {
	return make_index_id(space_id, 15);
}

u32 get_space_id_from(u32 num) {
	return (num << 6) >> 15;
}

u32 get_index_id_from(u32 num) {
	return (num << 2) >> 28;
}

}

/**
 * Function for getting sqlite connection object from lua stack
*/
static inline
sqlite3 *lua_check_sqliteconn(struct lua_State *L, int index)
{
	sqlite3 *conn = *(sqlite3 **) luaL_checkudata(L, index, sqlitelib_name);
	if (conn == NULL)
		luaL_error(L, "Attempt to use closed connection");
	return conn;
}

extern "C" {

/**
 * Callback function for sqlite3_exec function. It fill
 * sql_result structure with data, received from database.
 * 
 * @param data Pointer to struct sql_result
 * @param cols Number of columns in result
 * @param values One concrete row from result
 * @param names Array of column names
*/
int
sql_callback(void *data, int cols, char **values, char **names) {
	sql_result *res = (sql_result *)data;
	if (res->names == NULL) {
		res->names = (char **)malloc(sizeof(char *) * cols);
		for (int i = 0; i < cols; ++i) {
			int tmp = (strlen(names[i]) + 1) * sizeof(char);
			res->names[i] = (char *)malloc(tmp);
			memset(res->names[i], 0, tmp);
			memcpy(res->names[i], names[i], tmp);
		}
		res->cols = cols;
	}
	res->rows++;
	if (res->values == NULL) {
		res->values = (char ***)malloc(sizeof(char **) * 1);
	} else {
		res->values = (char ***)realloc((void *)res->values, sizeof(char **) * res->rows);
	}
	int cur = res->rows - 1;
	res->values[cur] = (char **)malloc(sizeof(char *) * cols);
	for (int i = 0; i < cols; ++i) {
		int tmp = 0;
		if (values[i] == NULL) {
			tmp = sizeof(char) * strlen("NULL");
		} else tmp = sizeof(char) * strlen(values[i]);
		++tmp;
		res->values[cur][i] = (char *)malloc(tmp);
		memset(res->values[cur][i], 0, tmp);
		if (values[i] == NULL) {
			memcpy(res->values[cur][i], "NULL", tmp);
		} else memcpy(res->values[cur][i], values[i], tmp);
	}
	return 0;
}

void on_commit_space(struct trigger * /*trigger*/, void * event) {
	static const char *__func_name = "on_commit_space";
	say_debug("%s():\n", __func_name);
	struct txn *txn = (struct txn *) event;
	struct txn_stmt *stmt = txn_stmt(txn);
	struct tuple *old_tuple = stmt->old_tuple;
	struct tuple *new_tuple = stmt->new_tuple;
	sqlite3 *db = global_db;
	Hash *tblHash = &db->aDb[0].pSchema->tblHash;
	Schema *pSchema = db->aDb[0].pSchema;
	if (old_tuple != NULL) {
		say_debug("%s(): old_tuple != NULL\n", __func_name);
		Table *table = get_trntl_table_from_tuple(old_tuple, db, pSchema);
		if (!table) {
			say_debug("%s(): error while getting table\n", __func_name);
			return;
		}
		Table *schema_table = (Table *)sqlite3HashFind(tblHash, table->zName);
		if (!schema_table) {
			say_debug("%s(): table was not found\n", __func_name);
			sqlite3DbFree(db, table);
			return;
		}
		sqlite3HashInsert(tblHash, table->zName, NULL);
		sqlite3DbFree(db, table);
		sqlite3DbFree(db, schema_table);
	}
	if (new_tuple != NULL) {
		say_debug("%s(): new_tuple != NULL\n", __func_name);
		Table *table = get_trntl_table_from_tuple(new_tuple, db, pSchema);
		(void)table;
		sqlite3HashInsert(tblHash, table->zName, table);
	}
}

void on_replace_space(struct trigger * /*trigger*/, void * event) {
	static const char *__func_name = "on_replace_space";
	say_debug("%s():\n", __func_name);
	struct trigger *on_commit = (struct trigger *) region_alloc0(&fiber()->gc, sizeof(struct trigger));
	*on_commit = {
		RLIST_LINK_INITIALIZER, on_commit_space, NULL, NULL
	};
	struct txn *txn = (struct txn *) event;
	trigger_add(&txn->on_commit, on_commit);
}

void on_commit_index(struct trigger * /*trigger*/, void * event) {
	static const char *__func_name = "on_commit_index";
	say_debug("%s():\n", __func_name);
	struct txn *txn = (struct txn *) event;
	struct txn_stmt *stmt = txn_stmt(txn);
	struct tuple *old_tuple = stmt->old_tuple;
	struct tuple *new_tuple = stmt->new_tuple;
	sqlite3 *db = global_db;
	// Hash *tblHash = &db->aDb[0].pSchema->tblHash;
	Hash *idxHash = &db->aDb[0].pSchema->idxHash;
	// Schema *pSchema = db->aDb[0].pSchema;
	if (old_tuple != NULL) {
		say_debug("%s(): old_tuple != NULL\n", __func_name);
		bool ok;
		SIndex *index = get_trntl_index_from_tuple(old_tuple, db, NULL, ok);
		if (index == NULL) {
			say_debug("%s(): index is null\n", __func_name);
			return;
		}
		Table *table = index->pTable;
		SIndex *prev = NULL, *cur;
		ok = false;
		for (cur = table->pIndex; cur != NULL; prev = cur, cur = cur->pNext) {
			if (cur->tnum == index->tnum) {
				ok = true;
				if (!prev) {
					table->pIndex = cur->pNext;
					break;
				}
				if (!cur->pNext) {
					prev->pNext = NULL;
					break;
				}
				prev->pNext = cur->pNext;
				break;
			}
		}
		if (!ok) {
			say_debug("%s(): index was not found in sql schema\n", __func_name);
			sqlite3DbFree(db, index);
			return;
		}
		sqlite3HashInsert(idxHash, cur->zName, NULL);
		sqlite3DbFree(db, index);
		sqlite3DbFree(db, cur);
	}
	if (new_tuple != NULL) {
		say_debug("%s(): new_tuple != NULL\n", __func_name);
		bool ok;
		SIndex *index = get_trntl_index_from_tuple(new_tuple, db, NULL, ok);
		if (!index) {
			say_debug("%s(): error while getting index from tuple\n", __func_name);
			return;
		}
		Table *table = index->pTable;
		index->pNext = table->pIndex;
		table->pIndex = index;
		sqlite3HashInsert(idxHash, index->zName, index);
	}
}

void on_replace_index(struct trigger * /*trigger*/, void * event) {
	static const char *__func_name = "on_replace_index";
	say_debug("%s():\n", __func_name);
	struct trigger *on_commit = (struct trigger *) region_alloc0(&fiber()->gc, sizeof(struct trigger));
	*on_commit = {
		RLIST_LINK_INITIALIZER, on_commit_index, NULL, NULL
	};
	struct txn *txn = (struct txn *) event;
	trigger_add(&txn->on_commit, on_commit);
}

void prepare_to_open_db() {
	sql_tarantool_api_init(&global_trn_api);
	global_trn_api_is_ready = 1;
}

void connect_triggers() {
	struct space *space = space_cache_find(BOX_SPACE_ID);
	struct trigger *alter_space_on_replace_space = (struct trigger *)malloc(sizeof(struct trigger));
	memset(alter_space_on_replace_space, 0, sizeof(struct trigger));
	*alter_space_on_replace_space = {
		RLIST_LINK_INITIALIZER, on_replace_space, NULL, NULL
	};
	rlist_add_tail_entry(&space->on_replace, alter_space_on_replace_space, link);

	space = space_cache_find(BOX_INDEX_ID);
	struct trigger *alter_space_on_replace_index = (struct trigger *)malloc(sizeof(struct trigger));
	memset(alter_space_on_replace_index, 0, sizeof(struct trigger));
	*alter_space_on_replace_index = {
		RLIST_LINK_INITIALIZER, on_replace_index, NULL, NULL
	};
	rlist_add_tail_entry(&space->on_replace, alter_space_on_replace_index, link);
}

int make_connect_sqlite_db(const char *db_name, struct sqlite3 **db) {
	prepare_to_open_db();
	int rc = sqlite3_open(db_name, db);
	if (rc != SQLITE_OK) {
		return rc;
	}
	connect_triggers();
	return rc;
}

/**
 * Function for creating connection to sqlite database.
 * Pointer to connection object is put on lua stack with
 * name 'sqlite'.
 *
 * Params needed on stack:
 * - (char *) - name of database
*/

static int
lua_sql_connect(struct lua_State *L)
{
	if (lua_gettop(L) < 1) {
		return luaL_error(L, "Usage: sql.connect(<database_name>)");
	}
	const char *db_name = luaL_checkstring(L, 1);
	sqlite3 *db = NULL;

	int rc = make_connect_sqlite_db(db_name, &db);
	if (rc != SQLITE_OK) {
		luaL_error(L, "Error: error during opening database <%s>", db_name);
	}
	global_db = db;
	sqlite3 **ptr = (sqlite3 **)lua_newuserdata(L, sizeof(sqlite3 *));
	*ptr = db;
	luaL_getmetatable(L, sqlitelib_name);
	lua_setmetatable(L, -2);
	return 1;
}
}

/**
 * Function for pushing content of sql_result structure
 * on lua stack. It will be table with first row as names
 * and others as values. It push lua stack true and table of
 * results if all is ok and false else.
 *
 * @param L lua_State object on stack of which result must be pushed
 * @param res Object of struct sql_result from which data must be pushed
*/
static int
lua_sqlite_pushresult(struct lua_State *L, sql_result res)
{

	lua_createtable(L, 1, 1);
	int tmp = lua_gettop(L);
	lua_pushstring(L, "__serialize");
	lua_pushstring(L, "seq");
	lua_settable(L, tmp);


	//lua_setmetatable(L, tid);
	lua_createtable(L, 0, 1 + res.rows);
	int tid = lua_gettop(L);
	//adding array of names
	lua_createtable(L, res.cols, 0);
	lua_pushvalue(L, tmp);
	lua_setmetatable(L, -2);
	int names_id = lua_gettop(L);
	for (size_t i = 0; i < res.cols; ++i) {
		lua_pushstring(L, res.names[i]);
		lua_rawseti(L, names_id, i + 1);
	}
	lua_rawseti(L, tid, 1);
	for (size_t i = 0; i < res.rows; ++i) {
		lua_createtable(L, res.cols, 0);
		lua_pushvalue(L, tmp);
		lua_setmetatable(L, -2);
		int vals_id = lua_gettop(L);
		for (size_t j = 0; j < res.cols; ++j) {
			lua_pushstring(L, res.values[i][j]);
			lua_rawseti(L, vals_id, j + 1);
		}
		lua_rawseti(L, tid, i + 2);
	}
	return 1;
}

static int
lua_sqlite_close(struct lua_State *L)
{
	sqlite3 *db = lua_check_sqliteconn(L, 1);
	sqlite3_close(db);
	return 0;
}

/**
 * Function for executing SQL query.
 *
 * Params needed on stack:
 * - (char *) - SQL query
*/
static int
lua_sqlite_execute(struct lua_State *L)
{
	sqlite3 *db = lua_check_sqliteconn(L, 1);
	size_t len;
	const char *sql = lua_tolstring(L, 2, &len);

	char *errMsg = NULL;
	sql_result res;
	sql_result_init(&res);
	int rc = sqlite3_exec(db, sql, sql_callback, (void *)&res, &errMsg);
	if (rc != SQLITE_OK) {
		std::string tmp;
		try {
			if (errMsg != NULL) tmp = std::string(errMsg);
		} catch(...) { tmp = "Error during reading error message"; }
		sqlite3_free(errMsg);
		luaL_error(L, "Error: error while executing query `%s`\nError message: %s", sql, tmp.c_str());
	}
	int ret = lua_sqlite_pushresult(L, res);
	sql_result_free(&res);
	return ret;
}

/**
 * Function for initializing sqlite submodule
*/
void
box_lua_sqlite_init(struct lua_State *L)
{
	static const struct luaL_reg sqlite_methods [] = {
		{"execute",	lua_sqlite_execute},
		{"close",	lua_sqlite_close},
		{NULL, NULL}
	};
	luaL_newmetatable(L, sqlitelib_name);
	lua_pushvalue(L, -1);
	luaL_register(L, NULL, sqlite_methods);
	lua_setfield(L, -2, "__index");
	lua_pushstring(L, sqlitelib_name);
	lua_setfield(L, -2, "__metatable");
	lua_pop(L, 1);

	static const struct luaL_reg module_funcs [] = {
		{"connect", lua_sql_connect},
		{NULL, NULL}
	};
	luaL_register_module(L, "sql", module_funcs);
	lua_pop(L, 1);
}

extern "C" {

int trntl_cursor_create(void *self_, Btree *p, int iTable, int wrFlag,
                              struct KeyInfo *pKeyInfo, BtCursor *pCur) {
	static const char *__func_name = "trntl_cursor_create";
	sql_trntl_self *self = reinterpret_cast<sql_trntl_self *>(self_);

	for (int i = 0; i < self->cnt_cursors; ++i) {
		if (self->cursors[i]->brother == pCur) {
			say_debug("%s(): trying to reinit existing cursor\n", __func_name);
			return SQLITE_ERROR;
		}
	}
	u32 num;
	memcpy(&num, &iTable, sizeof(u32));
	TrntlCursor *c = new TrntlCursor();
	c->key = new char[2];
	char *key_end = mp_encode_array(c->key, 0);
	int index_id = 0;
	int type = ITER_ALL;
	int space_id = get_space_id_from(num);
	index_id = get_index_id_from(num) % 15;
	u32 tnum = make_index_id(space_id, index_id);
	SIndex *sql_index = NULL;
	for (int i = 0; i < self->cnt_indices; ++i) {
		if (self->indices[i]->tnum == tnum) {
			sql_index = self->indices[i];
			break;
		}
	}
	if (sql_index == NULL) {
		say_debug("%s(): sql_index not found, space_id = %d, index_id = %d\n", __func_name, space_id, index_id);
		delete[] c;
		delete[] c->key;
		return SQLITE_ERROR;
	}
	c->cursor = TarantoolCursor(p->db, space_id, index_id, type, c->key, key_end, sql_index);
	c->brother = pCur;
	pCur->trntl_cursor = (void *)c;
	pCur->pBtree = p;
	pCur->pBt = p->pBt;
	memcpy(&pCur->pgnoRoot, &iTable, sizeof(Pgno));
	pCur->iPage = -1;
	pCur->curFlags = wrFlag;
	pCur->pKeyInfo = pKeyInfo;
	pCur->eState = CURSOR_VALID;
	if (self->cnt_cursors == 0) {
		self->cursors = new TrntlCursor*[1];
	} else {
		TrntlCursor **tmp = new TrntlCursor*[self->cnt_cursors + 1];
		memcpy(tmp, self->cursors, sizeof(TrntlCursor *) * self->cnt_cursors);
		delete[] self->cursors;
		self->cursors = tmp;
	}
	self->cursors[self->cnt_cursors++] = c;
	return SQLITE_OK;
}

int trntl_cursor_close(void *self_, BtCursor *pCur) {
	static const char *__func_name = "trntl_cursor_first";
	sql_trntl_self *self = reinterpret_cast<sql_trntl_self *>(self_);
	if (!self) {
		say_debug("%s(): self must not be NULL\n", __func_name);
		return SQLITE_ERROR;
	}
	//BtShared *pBt = pCur->pBt;
	TrntlCursor *c = (TrntlCursor *)pCur->trntl_cursor;
	delete[] c->key;
	TrntlCursor **new_cursors = new TrntlCursor*[self->cnt_cursors - 1];
	for (int i = 0, j = 0; i < self->cnt_cursors; ++i) {
		if (self->cursors[i]->brother != pCur) {
			new_cursors[j++] = self->cursors[i];
		}
	}
	delete[] self->cursors;
	self->cnt_cursors--;
	self->cursors = new TrntlCursor*[self->cnt_cursors];
	memcpy(self->cursors, new_cursors, sizeof(TrntlCursor *) * (self->cnt_cursors));
	delete c;
	sqlite3_free(pCur->pKey);
  	pCur->pKey = 0;
  	pCur->eState = CURSOR_INVALID;
	//unlockBtreeIfUnused(pBt);
	return SQLITE_OK;
}

int trntl_cursor_move_to_unpacked(void *self_, BtCursor *pCur, UnpackedRecord *pIdxKey, i64 intKey, int biasRight, int *pRes, RecordCompare xRecordCompare) {
	static const char *__func_name = "trntl_cursor_move_to_unpacked";
	sql_trntl_self *self = reinterpret_cast<sql_trntl_self *>(self_);
	if (!self) {
		say_debug("%s(): self must not be NULL\n", __func_name);
		return SQLITE_ERROR;
	}
	if ((biasRight != 0) && (biasRight != 1)) {
		say_debug("%s(): biasRight must be 1 or 0, but equal %d\n", __func_name, biasRight);
		return SQLITE_ERROR;
	}
	TrntlCursor *c = (TrntlCursor *)pCur->trntl_cursor;
	return c->cursor.MoveToUnpacked(pIdxKey, intKey, pRes, xRecordCompare);
}

int trntl_cursor_first(void *self_, BtCursor *pCur, int *pRes) {
	static const char *__func_name = "trntl_cursor_first";
	sql_trntl_self *self = reinterpret_cast<sql_trntl_self *>(self_);
	if (!self) {
		say_debug("%s(): self must not be NULL\n", __func_name);
		return SQLITE_ERROR;
	}
	TrntlCursor *c = (TrntlCursor *)(pCur->trntl_cursor);
	return c->cursor.MoveToFirst(pRes);
}

int trntl_cursor_data_size(void *self_, BtCursor *pCur, u32 *pSize) {
	static const char *__func_name = "trntl_cursor_data_size";
	sql_trntl_self *self = reinterpret_cast<sql_trntl_self *>(self_);
	if (!self) {
		say_debug("%s(): self must not be NULL\n", __func_name);
		return SQLITE_ERROR;
	}
	TrntlCursor *c = (TrntlCursor *)(pCur->trntl_cursor);
	return c->cursor.DataSize(pSize);
}

const void *trntl_cursor_data_fetch(void *self_, BtCursor *pCur, u32 *pAmt) {
	static const char *__func_name = "trntl_cursor_data_fetch";
	sql_trntl_self *self = reinterpret_cast<sql_trntl_self *>(self_);
	if (!self) {
		say_debug("%s(): self must not be NULL\n", __func_name);
		return NULL;
	}
	TrntlCursor *c = (TrntlCursor *)(pCur->trntl_cursor);
	return c->cursor.DataFetch(pAmt);
}

int trntl_cursor_key_size(void *self_, BtCursor *pCur, i64 *pSize) {
	static const char *__func_name = "trntl_cursor_key_size";
	sql_trntl_self *self = reinterpret_cast<sql_trntl_self *>(self_);
	if (!self) {
		say_debug("%s(): self must not be NULL\n", __func_name);
		return SQLITE_ERROR;
	}
	TrntlCursor *c = (TrntlCursor *)(pCur->trntl_cursor);
	return c->cursor.KeySize(pSize);
}
 
const void *trntl_cursor_key_fetch(void *self_, BtCursor *pCur, u32 *pAmt) {
	static const char *__func_name = "trntl_cursor_key_fetch";
	sql_trntl_self *self = reinterpret_cast<sql_trntl_self *>(self_);
	if (!self) {
		say_debug("%s(): self must not be NULL\n", __func_name);
		return NULL;
	}
	TrntlCursor *c = (TrntlCursor *)(pCur->trntl_cursor);
	return c->cursor.KeyFetch(pAmt);
}

int trntl_cursor_next(void *self_, BtCursor *pCur, int *pRes) {
	static const char *__func_name = "trntl_cursor_next";
	sql_trntl_self *self = reinterpret_cast<sql_trntl_self *>(self_);
	if (!self) {
		say_debug("%s(): self must not be NULL\n", __func_name);
		return SQLITE_ERROR;
	}
	TrntlCursor *c = (TrntlCursor *)(pCur->trntl_cursor);
	return c->cursor.Next(pRes);
}

char check_num_on_tarantool_id(void *self, u32 num) {
	static const char *__func_name = "check_num_on_tarantool_id";
	if (self == NULL) {
		say_debug("%s(): self must not be NULL\n", __func_name);
		return 0;
	}
	u32 buf;
	buf = (num << 23) >> 23;
	if (buf) return 0;
	return !!(num & (1 << 30));
}

Table *get_trntl_table_from_tuple(box_tuple_t *tpl, sqlite3 *db, Schema *pSchema) {
	static const char *__func_name = "get_trntl_table_from_tuple";

	int cnt = box_tuple_field_count(tpl);
	if (cnt != 7) {
		say_debug("%s(): box_tuple_field_count not equal 7, but %d\n", __func_name, cnt);
		return NULL;
	}
	Table *table = NULL;
	table = reinterpret_cast<Table *>(sqlite3DbMallocZero(db, malloc_size(sizeof(Table))));
	if (db->mallocFailed) {
		say_debug("%s(): error while allocating memory for table\n", __func_name);
		return NULL;
	}
	table->pSchema = pSchema;
	table->iPKey = -1;
	table->tabFlags = TF_WithoutRowid | TF_HasPrimaryKey;

	const char *data = box_tuple_field(tpl, 0);
	int type = (int)mp_typeof(*data);
	MValue tbl_id = MValue::FromMSGPuck(&data);
	if (tbl_id.GetType() != MP_UINT) {
		say_debug("%s(): field[0] in tuple in SPACE must be uint, but is %d\n", __func_name, type);
		sqlite3DbFree(db, table);
		return NULL;
	}
	table->tnum = make_space_id(tbl_id.GetUint64());

	data = box_tuple_field(tpl, 2);
	type = (int)mp_typeof(*data);
	if (type != MP_STR) {
		say_debug("%s(): field[2] in tuple in SPACE must be string, but is %d\n", __func_name, type);
		sqlite3DbFree(db, table);
		return NULL;
	} else {
		size_t len;
		MValue buf = MValue::FromMSGPuck(&data);
		table->zName = sqlite3DbStrNDup(db, buf.GetStr(&len), len);
		if (db->mallocFailed) {
			say_debug("%s(): error while allocating memory for table name, = %s\n", __func_name, buf.GetStr());
			sqlite3DbFree(db, table);
			return NULL;
		}
	}

	//Get space format
	data = box_tuple_field(tpl, 6);
	uint32_t len = mp_decode_array(&data);

	int nCol = 0;
	for (uint32_t i = 0; i < len; ++i) {
		uint32_t map_size = mp_decode_map(&data);
		MValue colname, coltype;
		if (map_size != 2) {
			say_debug("%s(): map_size not equal 2, but %u\n", __func_name, map_size);
			sqlite3DbFree(db, table);
			return NULL;
		}
		for (uint32_t j = 0; j < map_size; ++j) {
			MValue key = MValue::FromMSGPuck(&data);
			MValue val = MValue::FromMSGPuck(&data);
			if ((key.GetType() != MP_STR) || (val.GetType() != MP_STR)) {
				say_debug("%s(): unexpected not string format\n", __func_name);
				sqlite3DbFree(db, table);
				return NULL;
			}
			char c = key.GetStr()[0];
			if ((c == 'n') || (c == 'N')) {
				//name
				colname = val;
			} else if ((c == 't') || (c == 'T')) {
				//type
				coltype = val;
			} else {
				say_debug("%s(): unknown string in space_format\n", __func_name);
				sqlite3DbFree(db, table);
				return NULL;
			}
		}
		if (colname.IsEmpty() || coltype.IsEmpty()) {
			say_debug("%s(): both name and type must be init\n", __func_name);
		}
		char c = coltype.GetStr()[0];
		const char *sql_type;
		switch(c) {
			case 'n': case 'N': {
				sql_type = "REAL";
				break;
			}
			case 's': case 'S': {
				sql_type = "TEXT";
				break;
			}
			default: {
				sql_type = "BLOB";
				break;
			}
		}
		size_t len;
		char zName[256];
		memset(zName, 0, sizeof(zName));
		colname.GetStr(&len);
		memcpy(zName, colname.GetStr(), len);

		Parse p;
		Token t;
		memset(&p, 0, sizeof(p));
		memset(&t, 0, sizeof(t));
		p.db = db;
		p.pNewTable = table;
		t.z = zName;
		t.n = len;
		sqlite3AddColumn(&p, &t);
		if (db->mallocFailed) {
			say_debug("%s(): malloc failed while allocating memory for new table, size = %d\n", __func_name, nCol);
			sqlite3DbFree(db, table);
			return NULL;
		}
		t.z = sql_type;
		t.n= strlen(sql_type);
		sqlite3AddColumnType(&p, &t);
		Column *cur = &(table->aCol[table->nCol - 1]);
		cur->zType = sqlite3DbStrDup(db, sql_type);
	}
	table->nRef = 1;
	table->nRowLogEst = box_index_len(tbl_id.GetUint64(), 0);
	table->szTabRow = ESTIMATED_ROW_SIZE;

	return table;
}

SIndex *get_trntl_index_from_tuple(box_tuple_t *index_tpl, sqlite3 *db, Table *table, bool &ok) {
	static const char *__func_name = "get_trntl_index_from_tuple";
	ok = false;

	int cnt = box_tuple_field_count(index_tpl);
	if (cnt != 6) {
		say_debug("%s(): box_tuple_field_count not equal 6, but %d, for next index\n", __func_name, cnt);
		return NULL;
	}

	//---- SPACE ID ----

	const char *data = box_tuple_field(index_tpl, 0);
	int type = (int)mp_typeof(*data);
	MValue space_id = MValue::FromMSGPuck(&data);
	if (space_id.GetType() != MP_UINT) {
		say_debug("%s(): field[0] in tuple in INDEX must be uint, but is %d\n", __func_name, type);
		return NULL;
	}

	if (!table) {
		Schema *pSchema = db->aDb[0].pSchema;
		struct space *space = space_cache_find(space_id.GetUint64());
		table = (Table *)sqlite3HashFind(&pSchema->tblHash, space_name(space));
		if (!table) {
			say_debug("%s(): space with id %llu was not found\n", __func_name, space_id.GetUint64());
			return NULL;
		}
	}

	u32 tbl_id = get_space_id_from(table->tnum);
	if (space_id.GetUint64() != tbl_id) {
		ok = true;
		return NULL;
	}

	SIndex *index = NULL;
	char *extra = NULL;
	int extra_sz = 0;
	char zName[256];

	//---- INDEX ID ----

	data = box_tuple_field(index_tpl, 1);
	type = (int)mp_typeof(*data);
	MValue index_id = MValue::FromMSGPuck(&data);
	if (index_id.GetType() != MP_UINT) {
		say_debug("%s(): field[1] in tuple in INDEX must be uint, but is %d\n", __func_name, type);
		return NULL;
	}

	//---- INDEX NAME ----

	data = box_tuple_field(index_tpl, 2);
	type = (int)mp_typeof(*data);
	if (type != MP_STR) {
		say_debug("%s(): field[2] in tuple in INDEX must be string, but is %d\n", __func_name, type);
		return NULL;
	} else {
		memset(zName, 0, 256);
		size_t len = 0;
		MValue buf = MValue::FromMSGPuck(&data);
		buf.GetStr(&len);
		sprintf(zName, "%d_%d_", (int)space_id.GetUint64(), (int)index_id.GetUint64());
		memcpy(zName + strlen(zName), buf.GetStr(), len);
		extra_sz += strlen(zName) + 1;
	}

	index = sqlite3AllocateIndexObject(db, table->nCol, extra_sz, &extra);
	if (db->mallocFailed) {
		say_debug("%s(): error while allocating memory for index\n", __func_name);
		return NULL;
	}
	index->pTable = table;
	index->pSchema = table->pSchema;
	index->isCovering = 1;
	index->noSkipScan = 1;
	if (index_id.GetUint64()) {
		index->idxType = 0;
	} else {
		index->idxType = 2;
	}
	index->tnum = make_index_id(space_id.GetUint64(), index_id.GetUint64());
	index->zName = extra;
	{
		int len = strlen(zName);
		memcpy(index->zName, zName, len);
		index->zName[len] = 0;
	}
	//index->zName = sqlite3DbStrDup(db, zName);
	// if (db->mallocFailed) {
	// 	say_debug("%s(): error while allocating memory for index name, = %s\n", __func_name, buf.GetStr());
	// 	return NULL;
	// }

	//---- SORT ORDER ----

	//index->aSortOrder = reinterpret_cast<u8 *>(sqlite3DbMallocZero(db, sizeof(u8)));
	index->aSortOrder[0] = 0;
	index->szIdxRow = ESTIMATED_ROW_SIZE;
	index->nColumn = table->nCol;
	index->onError = OE_Abort;
	//index->azColl = reinterpret_cast<char **>(sqlite3DbMallocZero(db, sizeof(char *) * table->nCol));
	for (uint32_t j = 0; j < table->nCol; ++j) {
		index->azColl[j] = reinterpret_cast<char *>(sqlite3DbMallocZero(db, malloc_size(sizeof(char) * (strlen("BINARY") + 1))));
		memcpy(index->azColl[j], "BINARY", strlen("BINARY"));
	}

	//---- TYPE ----

	data = box_tuple_field(index_tpl, 3);
	type = (int)mp_typeof(*data);
	if (type != MP_STR) {
		say_debug("%s(): field[3] in tuple in INDEX must be string, but is %d\n", __func_name, type);
		return NULL;
	} else {
		MValue buf = MValue::FromMSGPuck(&data);
		if ((buf.GetStr()[0] == 'T') || (buf.GetStr()[0] == 't')) {
			index->bUnordered = 0;
		} else {
			index->bUnordered = 1;
		}
	}

	//---- UNIQUE ----

	data = box_tuple_field(index_tpl, 4);
	int map_size = mp_decode_map(&data);
	if (map_size != 1) {
		say_debug("%s(): field[4] map size in INDEX must be 1, but %u\n", __func_name, map_size);
		return NULL;
	}
	MValue key, value;
	for (uint32_t j = 0; j < map_size; ++j) {
		key = MValue::FromMSGPuck(&data);
		value = MValue::FromMSGPuck(&data);
		if (key.GetType() != MP_STR) {
			say_debug("%s(): field[4][%u].key must be string, but type %d\n", __func_name, j, key.GetType());
			return NULL;
		}
		if (value.GetType() != MP_BOOL) {
			say_debug("%s(): field[4][%u].value must be bool, but type %d\n", __func_name, j, value.GetType());
			return NULL;
		}
	}
	if ((key.GetStr()[0] == 'u') || (key.GetStr()[0] == 'U')) {
		if (value.GetBool()) {
			if (index->idxType != 2) index->idxType = 1;
			index->uniqNotNull = 1;
		}
		else index->uniqNotNull = 0;
	}

	//---- INDEX FORMAT ----

	data = box_tuple_field(index_tpl, 5);
	MValue idx_cols = MValue::FromMSGPuck(&data);
	if (idx_cols.GetType() != MP_ARRAY) {
		say_debug("%s(): field[5] in INDEX must be array, but type is %d\n", __func_name, idx_cols.GetType());
		return NULL;
	}
	//index->aiColumn = reinterpret_cast<i16 *>(sqlite3DbMallocZero(db, sizeof(i16) * table->nCol));
	index->nKeyCol = idx_cols.Size();
	for (uint32_t j = 0, sz = idx_cols.Size(); j < sz; ++j) {
		i16 num = idx_cols[j][0][0]->GetUint64();
		index->aiColumn[j] = num;
	}
	for (uint32_t j = 0, start = idx_cols.Size(); j < table->nCol; ++j) {
		bool used = false;
		for (uint32_t k = 0, sz = idx_cols.Size(); k < sz; ++k) {
			if (index->aiColumn[k] == j) {
				used = true;
				break;
			}
		}
		if (used) continue;
		index->aiColumn[start++] = j;
	}

	//index->aiRowLogEst = reinterpret_cast<LogEst *>(sqlite3DbMallocZero(db, sizeof(LogEst) * index->nKeyCol));
	for (int i = 0; i < index->nKeyCol; ++i) index->aiRowLogEst[i] = table->nRowLogEst;

	ok = true;
	return index;
}

Hash get_trntl_spaces(void *self_, sqlite3 *db, char **pzErrMsg, Schema *pSchema, Hash *idxHash_) {
	static const char *__func_name = "get_trntl_spaces";

	sql_trntl_self *self = reinterpret_cast<sql_trntl_self *>(self_);
	say_debug("%s(): self = %d, db = %d\n", __func_name, (int)(self != NULL), (int)(db != NULL));
	if (__func_name == NULL) {
		sqlite3SetString(pzErrMsg, db, "__func_name == NULL");
	}

	Hash tblHash;
	Hash idxHash;
	memset(&tblHash, 0, sizeof(Hash));
	memset(&idxHash, 0, sizeof(Hash));

	char key[2], *key_end = mp_encode_array(key, 0);
	box_iterator_t *it = box_index_iterator(BOX_SPACE_ID, 0, ITER_ALL, key, key_end);
	box_tuple_t *tpl = NULL;
	SIndex **new_indices = NULL;
	box_txn_begin();


	while(1) {
		if (box_iterator_next(it, &tpl)) {
			say_debug("%s(): box_iterator_next return not 0\n", __func_name);
			goto __get_trntl_spaces_end_bad;
		}
		if (!tpl) {
			break;
		}
		Table *table = get_trntl_table_from_tuple(tpl, db, pSchema);
		if (table == NULL) return tblHash;

		//----Indices----

		box_iterator_t *index_it = box_index_iterator(BOX_INDEX_ID, 0, ITER_ALL, key, key_end);
		box_tuple_t *index_tpl = NULL;
		while(1) {
			MValue key, value, idx_cols, index_id, space_id;
			SIndex *index = NULL;

			if (box_iterator_next(index_it, &index_tpl)) {
				say_debug("%s(): box_iterator_next return not 0 for next index\n", __func_name);
				goto __get_trntl_spaces_index_bad;
			}
			if (!index_tpl) {
				break;
			}
			
			bool ok;
			index = get_trntl_index_from_tuple(index_tpl, db, table, ok);
			if (index == NULL) {
				if (ok) continue;
				return tblHash;
			}

			sqlite3HashInsert(&idxHash, index->zName, index);
			if (table->pIndex) {
				index->pNext = table->pIndex;
			}
			table->pIndex = index;

			new_indices = new SIndex*[self->cnt_indices + 1];
			memcpy(new_indices, self->indices, self->cnt_indices * sizeof(SIndex *));
			new_indices[self->cnt_indices] = index;
			self->cnt_indices++;
			if (self->indices) delete[] self->indices;
			self->indices = new_indices;

			continue;
__get_trntl_spaces_index_bad:
			box_iterator_free(index_it);
			sqlite3DbFree(db, table);
			if (index->aSortOrder) sqlite3DbFree(db, index->aSortOrder);
			if (index->aiColumn) sqlite3DbFree(db, index->aiColumn);
			if (index->azColl) {
				for (uint32_t j = 0; j < index->nColumn; ++j) {
					sqlite3DbFree(db, index->azColl[j]);
				}
				sqlite3DbFree(db, index->azColl);
			}
			if (index) sqlite3DbFree(db, index);
			goto __get_trntl_spaces_end_bad;
		}
		box_iterator_free(index_it);

		sqlite3HashInsert(&tblHash, table->zName, table);
	}

	box_iterator_free(it);
	box_txn_commit();
	*idxHash_ = idxHash;
	return tblHash;
__get_trntl_spaces_end_bad:
	box_txn_rollback();
	box_iterator_free(it);
	return tblHash;
}

void sql_tarantool_api_init(sql_tarantool_api *ob) {
	ob->get_trntl_spaces = &get_trntl_spaces;
	sql_trntl_self *self = new sql_trntl_self;
	ob->self = self;
	self->cursors = NULL;
	self->cnt_cursors = 0;
	self->indices = NULL;
	self->cnt_indices = 0;
	ob->trntl_cursor_create = trntl_cursor_create;
	ob->trntl_cursor_first = trntl_cursor_first;
	ob->trntl_cursor_data_size = trntl_cursor_data_size;
	ob->trntl_cursor_data_fetch = trntl_cursor_data_fetch;
	ob->trntl_cursor_next = trntl_cursor_next;
	ob->trntl_cursor_close = trntl_cursor_close;
	ob->check_num_on_tarantool_id = check_num_on_tarantool_id;
	ob->trntl_cursor_move_to_unpacked = trntl_cursor_move_to_unpacked;
	ob->trntl_cursor_key_size = trntl_cursor_key_size;
	ob->trntl_cursor_key_fetch = trntl_cursor_key_fetch;
}

}