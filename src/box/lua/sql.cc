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
#include "sqlite3.h"
#include "sqliteInt.h"
#include "sql.h"
}

#include "lua/utils.h"
#include <cstdlib>
#include <cstring>
#include <string>

static const char *sqlitelib_name = "sqlite";

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
		for (int i = 0; i < ob->cols; ++i) std::free(ob->names[i]);
		std::free(ob->names);
	}
	if (ob->values != NULL) {
		for (int i = 0; i < ob->rows; ++i) {
			for (int j = 0; j < ob->cols; ++j) {
				std::free(ob->values[i][j]);
			}
			std::free(ob->values[i]);
		}
		std::free(ob->values);
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
		res->names = (char **)std::malloc(sizeof(char *) * cols);
		for (int i = 0; i < cols; ++i) {
			int tmp = std::strlen(names[i]) * sizeof(char);
			res->names[i] = (char *)malloc(tmp);
			std::memcpy(res->names[i], names[i], tmp);
		}
		res->cols = cols;
	}
	res->rows++;
	if (res->values == NULL) {
		res->values = (char ***)std::malloc(sizeof(char **) * 1);
	} else {
		res->values = (char ***)std::realloc((void *)res->values, sizeof(char **) * res->rows);
	}
	int cur = res->rows - 1;
	res->values[cur] = (char **)std::malloc(sizeof(char *) * cols);
	for (int i = 0; i < cols; ++i) {
		int tmp = sizeof(char) * std::strlen(values[i]);
		res->values[cur][i] = (char *)std::malloc(tmp);
		std::memcpy(res->values[cur][i], values[i], tmp);
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
static int
lua_sql_connect(struct lua_State *L)
{
	if (lua_gettop(L) < 1) {
		return luaL_error(L, "Usage: sql.connect(<database_name>)");
	}
	const char *db_name = luaL_checkstring(L, 1);
	sqlite3 *db = NULL;
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