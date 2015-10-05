extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <sqlite3.h>
#include "sqliteInt.h"
#include "sql.h"
}

#include "lua/utils.h"
#include <cstdlib>
#include <cstring>

struct SQLRes {
	char **names;
	char ***values;
	int cols;
	int rows;
};

void SQLRes_init(SQLRes *ob) {
	ob->names = NULL;
	ob->values = NULL;
	ob->cols = 0;
	ob->rows = 0;
}

int sql_callback(void *data, int cols, char **values, char **names) {
	SQLRes *res = (SQLRes *)data;
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

static int lbox_net_sqlite_connect(struct lua_State *L)
{
	if (lua_gettop(L) < 1) {
		return luaL_error(L, "Usage: sqlite.connect(<database_name>)");
	}
	const char *db_name = luaL_checkstring(L, 1);
	sqlite3 *db = NULL;
	int rc = sqlite3_open(db_name, &db);
	if (rc != SQLITE_OK) {
		luaL_error(L, "Error: error during opening database <%s>", db_name);
	}
	sqlite3 **ptr = (sqlite3 **)lua_newuserdata(L, sizeof(sqlite3 *));
	*ptr = db;
	luaL_getmetatable(L, "sqlite");
	lua_setmetatable(L, -2);
	return 1;
}

static inline sqlite3 *lua_check_sqliteconn(struct lua_State *L, int index)
{
	sqlite3 *conn = *(sqlite3 **) luaL_checkudata(L, index, "sqlite");
	if (conn == NULL)
		luaL_error(L, "Attempt to use closed connection");
	return conn;
}

static int lua_sqlite_pushresult(struct lua_State *L, SQLRes res)
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

static int lua_sqlite_execute(struct lua_State *L)
{
	sqlite3 *db = lua_check_sqliteconn(L, 1);
	size_t len;
	const char *sql = lua_tolstring(L, 2, &len);

	char *errMsg = NULL;
	SQLRes res;
	SQLRes_init(&res);
	int rc = sqlite3_exec(db, sql, sql_callback, (void *)&res, &errMsg);
	if (rc != SQLITE_OK) {
		sqlite3_free(errMsg);
		luaL_error(L, "Error: error while executing query `%s`\nError message: ", sql);
	}
	//lua_pushboolean(L, 1);
	//return 1;
	return lua_sqlite_pushresult(L, res);
}

void box_lua_sqlite_init(struct lua_State *L)
{
	static const struct luaL_reg sqlite_methods [] = {
		{"execute",	lua_sqlite_execute},
		//{"close",	lua_sqlite_close},
		{NULL, NULL}
	};
	luaL_newmetatable(L, "sqlite");
	lua_pushvalue(L, -1);
	luaL_register(L, NULL, sqlite_methods);
	lua_setfield(L, -2, "__index");
	lua_pushstring(L, "sqlite");
	lua_setfield(L, -2, "__metatable");
	lua_pop(L, 1);

	//lua_newtable(L);
	static const struct luaL_reg module_funcs [] = {
		{"connect", lbox_net_sqlite_connect},
		{NULL, NULL}
	};
	luaL_register_module(L, "sql", module_funcs);
	lua_pop(L, 1);
}