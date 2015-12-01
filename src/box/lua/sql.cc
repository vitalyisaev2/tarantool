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
} sql_trntl_self;

/** Struct for containing data, received from sqlite database. */
struct sql_result {
	/** Array of names as strings */
	char **names;
	/** Two-dimensional array of column values as strings */
	char ***values;
	/** Number of columns and also size of names array */
	int cols;
	/** Number of rows in values array */
	int rows;
};

extern "C" {
void sql_tarantool_api_init(sql_tarantool_api *ob);

Hash get_trntl_spaces(void *self_, sqlite3 *db, char **pzErrMsg, Schema *pSchema);

int trntl_cursor_create(void *self_, Btree *p, int iTable, int wrFlag,
                              struct KeyInfo *pKeyInfo, BtCursor *pCur);

int trntl_cursor_first(void *self_, BtCursor *pCur, int *pRes);

int trntl_cursor_data_size(void *self_, BtCursor *pCur, u32 *pSize);

const void *trntl_cursor_data_fetch(void *self_, BtCursor *pCur, u32 *pAmt);

int trntl_cursor_next(void *self, BtCursor *pCur, int *pRes);

int trntl_cursor_close(void *self, BtCursor *pCur);
}

/** 
 * Constructor function for struct sql_result. Must be called
 * before any actions with sql_result object.
 *
 * struct sql_result res;
 * sql_result_init(&res);
 * //......any actions with res
 * sql_result_free(&res);
 */
static void
sql_result_init(sql_result *ob) {
	ob->names = NULL;
	ob->values = NULL;
	ob->cols = 0;
	ob->rows = 0;
}

/** 
 * Destructor function for struct sql_result. Must be called
 * after all actions with sql_result object.
 *
 * struct sql_result res;
 * sql_result_init(&res);
 * //......any actions with res
 * sql_result_free(&res);
 */
static void
sql_result_free(sql_result *ob) {
	if (ob->names != NULL) {
		for (int i = 0; i < ob->cols; ++i) free(ob->names[i]);
		free(ob->names);
	}
	if (ob->values != NULL) {
		for (int i = 0; i < ob->rows; ++i) {
			for (int j = 0; j < ob->cols; ++j) {
				free(ob->values[i][j]);
			}
			free(ob->values[i]);
		}
		free(ob->values);
	}
}

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
			int tmp = strlen(names[i]) * sizeof(char);
			res->names[i] = (char *)malloc(tmp);
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
		int tmp = sizeof(char) * strlen(values[i]);
		res->values[cur][i] = (char *)malloc(tmp);
		memcpy(res->values[cur][i], values[i], tmp);
	}
	return 0;
}

/**
 * Function for creating connection to sqlite database.
 * Pointer to connection object is put on lua stack with
 * name 'sqlite'.
 *
 * Params needed on stack:
 * - (char *) - name of database
*/

extern "C" {
static int
lua_sql_connect(struct lua_State *L)
{
	if (lua_gettop(L) < 1) {
		return luaL_error(L, "Usage: sql.connect(<database_name>)");
	}
	const char *db_name = luaL_checkstring(L, 1);
	sqlite3 *db = NULL;

	sql_tarantool_api_init(&global_trn_api);
	global_trn_api_is_ready = 1;

	int rc = sqlite3_open(db_name, &db);
	if (rc != SQLITE_OK) {
		luaL_error(L, "Error: error during opening database <%s>", db_name);
	}
	sqlite3 **ptr = (sqlite3 **)lua_newuserdata(L, sizeof(sqlite3 *));
	*ptr = db;
	luaL_getmetatable(L, sqlitelib_name);
	lua_setmetatable(L, -2);
	return 1;
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
	lua_createtable(L, 0, 1 + res.rows);
	int tid = lua_gettop(L);
	//adding array of names
	lua_createtable(L, res.cols, 0);
	int names_id = lua_gettop(L);
	for (size_t i = 0; i < res.cols; ++i) {
		lua_pushstring(L, res.names[i]);
		lua_rawseti(L, names_id, i + 1);
	}
	lua_rawseti(L, tid, 1);
	for (size_t i = 0; i < res.rows; ++i) {
		lua_createtable(L, res.cols, 0);
		int vals_id = lua_gettop(L);
		for (size_t j = 0; j < res.cols; ++j) {
			lua_pushstring(L, res.values[i][j]);
			lua_rawseti(L, vals_id, j + 1);
		}
		lua_rawseti(L, tid, i + 2);
	}
	lua_pushboolean(L, 1);
	return 2;
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
		//{"close",	lua_sqlite_close},
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
	TrntlCursor *c = new TrntlCursor();
	c->key = new char[2];
	char *key_end = mp_encode_array(c->key, 0);
	int index_id = 0;
	int type = ITER_ALL;
	c->cursor = TarantoolCursor(p->db, iTable, index_id, type, c->key, key_end);
	c->brother = pCur;
	pCur->trntl_cursor = (void *)c;
	pCur->pBtree = p;
	pCur->pBt = p->pBt;
	pCur->pgnoRoot = (Pgno)iTable;
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

Hash get_trntl_spaces(void *self_, sqlite3 *db, char **pzErrMsg, Schema *pSchema) {
	static const char *__func_name = "get_trntl_spaces";

	sql_trntl_self *self = reinterpret_cast<sql_trntl_self *>(self_);
	say_debug("%s(): self = %d, db = %d\n", __func_name, (int)(self != NULL), (int)(db != NULL));
	if (__func_name == NULL) {
		sqlite3SetString(pzErrMsg, db, "__func_name == NULL");
	}

	Hash tblHash;
	memset(&tblHash, 0, sizeof(tblHash));

	char key[2], *key_end = mp_encode_array(key, 0);
	box_iterator_t *it = box_index_iterator(BOX_SPACE_ID, 0, ITER_ALL, key, key_end);
	box_tuple_t *tpl = NULL;
	box_txn_begin();


	while(1) {
		if (box_iterator_next(it, &tpl)) {
			say_debug("%s(): box_iterator_next return not 0\n", __func_name);
			goto __get_trntl_spaces_end_bad;
		}
		if (!tpl) {
			break;
		}
		int cnt = box_tuple_field_count(tpl);
		if (cnt != 7) {
			say_debug("%s(): box_tuple_field_count not equal 7, but %d\n", __func_name, cnt);
			goto __get_trntl_spaces_end_bad;
		}

		Table *table = NULL;
		table = reinterpret_cast<Table *>(sqlite3DbMallocZero(db, sizeof(Table)));
		if (db->mallocFailed) {
			say_debug("%s(): error while allocating memory for table\n", __func_name);
			goto __get_trntl_spaces_end_bad;
		}
		table->pSchema = pSchema;
		table->iPKey = -1;

		const char *data = box_tuple_field(tpl, 0);
		int type = (int)mp_typeof(*data);
		MValue tbl_id = MValue::FromMSGPuck(&data);
		if (tbl_id.GetType() != MP_UINT) {
			say_debug("%s(): field[0] in tuple in SPACE must be uint, but is %d\n", __func_name, type);
			sqlite3DbFree(db, table);
			goto __get_trntl_spaces_end_bad;
		}
		table->tnum = -tbl_id.GetUint64();

		data = box_tuple_field(tpl, 2);
		type = (int)mp_typeof(*data);
		if (type != MP_STR) {
			say_debug("%s(): field[2] in tuple in SPACE must be string, but is %d\n", __func_name, type);
			sqlite3DbFree(db, table);
			goto __get_trntl_spaces_end_bad;
		} else {
			size_t len;
			MValue buf = MValue::FromMSGPuck(&data);
			table->zName = sqlite3DbStrNDup(db, buf.GetStr(&len), len);
			if (db->mallocFailed) {
				say_debug("%s(): error while allocating memory for table name, = %s\n", __func_name, buf.GetStr());
				sqlite3DbFree(db, table);
				goto __get_trntl_spaces_end_bad;
			}
		}

		
		//Get space format
		data = box_tuple_field(tpl, 6);
		uint32_t len = mp_decode_array(&data);

		Column *cols = NULL;
		int nCol = 0;

		for (uint32_t i = 0; i < len; ++i) {
			uint32_t map_size = mp_decode_map(&data);
			MValue colname, coltype;
			if (map_size != 2) {
				say_debug("%s(): map_size not equal 2, but %u\n", __func_name, map_size);
				sqlite3DbFree(db, table);
				goto __get_trntl_spaces_end_bad;
			}
			for (uint32_t j = 0; j < map_size; ++j) {
				MValue key = MValue::FromMSGPuck(&data);
				MValue val = MValue::FromMSGPuck(&data);
				if ((key.GetType() != MP_STR) || (val.GetType() != MP_STR)) {
					say_debug("%s(): unexpected not string format\n", __func_name);
					sqlite3DbFree(db, table);
					goto __get_trntl_spaces_end_bad;
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
					goto __get_trntl_spaces_end_bad;
				}
			}
			if (colname.IsEmpty() || coltype.IsEmpty()) {
				say_debug("%s(): both name and type must be init\n", __func_name);
			}
			char c = coltype.GetStr()[0];
			const char *sql_type;
			int affinity;
			switch(c) {
				case 'n': case 'N': {
					sql_type = "REAL";
					affinity = SQLITE_AFF_REAL;
					break;
				}
				case 's': case 'S': {
					sql_type = "TEXT";
					affinity = SQLITE_AFF_TEXT;
					break;
				}
				default: {
					sql_type = "BLOB";
					affinity = SQLITE_AFF_BLOB;
					break;
				}
			}
			if (nCol) {
				cols = reinterpret_cast<Column *>(sqlite3DbRealloc(db, cols, (nCol + 1) * sizeof(Column)));
				memset(cols + nCol, 0, sizeof(Column));
			} else {
				cols = reinterpret_cast<Column *>(sqlite3DbMallocZero(db, sizeof(Column)));
			}
			if (db->mallocFailed) {
				say_debug("%s(): malloc failed while allocating memory for new table, size = %d\n", __func_name, nCol);
				sqlite3DbFree(db, table);
				goto __get_trntl_spaces_end_bad;
			}
			Column *cur = cols + nCol;
			nCol++;
			size_t len;
			cur->zName = sqlite3DbStrNDup(db, colname.GetStr(&len), len);
			cur->zType = sqlite3DbStrDup(db, sql_type);
			cur->affinity = affinity;
		}
		table->aCol = cols;
		table->nCol = nCol;
		table->nRef = 1;
		sqlite3HashInsert(&tblHash, table->zName, table);
	}

	box_iterator_free(it);
	box_txn_commit();
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
	ob->trntl_cursor_create = trntl_cursor_create;
	ob->trntl_cursor_first = trntl_cursor_first;
	ob->trntl_cursor_data_size = trntl_cursor_data_size;
	ob->trntl_cursor_data_fetch = trntl_cursor_data_fetch;
	ob->trntl_cursor_next = trntl_cursor_next;
	ob->trntl_cursor_close = trntl_cursor_close;
}

}