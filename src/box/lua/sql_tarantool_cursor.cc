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

#include "sql_tarantool_cursor.h"

int GetSerialTypeNum(u64 number) {
	if (number == 0) return 8;
	if (number == 1) return 9;
	int content_size = sqlite3VarintLen(number);
	if (content_size <= 4) return content_size;
	switch(content_size) {
		case 6: return 5;
		case 8: return 6;
		default: return -1;
	}
}

int GetSerialTypeNum(double) {
	return 7;
}

int GetSerialTypeNum(i64 number) {
	u64 tmp;
	memcpy(&tmp, &number, sizeof(u64));
	return GetSerialTypeNum(tmp);
}

int GetSerialTypeStr(size_t len) {
	return 2 * len + 13;
}

int GetSerialTypeBin(size_t len) {
	return 2 * len + 12;
}

int PutVarintDataNum(unsigned char *data, u64 n) {
	int st = GetSerialTypeNum(n);
	if ((st == 8) || (st == 9)) {
		return 0;
	}
	Mem mem;
	memset(&mem, 0, sizeof(Mem));
	memcpy((void *)&mem.u.i, (const void *)&n, sizeof(i64));
	mem.flags = MEM_Int;
	return sqlite3VdbeSerialPut(data, &mem, st);
}

int PutVarintDataNum(unsigned char *data, i64 n) {
	u64 tmp;
	memcpy(&tmp, &n, sizeof(u64));
	return PutVarintDataNum(data, tmp);
}

int PutVarintDataNum(unsigned char *data, double n) {
	Mem tmp;
	memset(&tmp, 0, sizeof(Mem));
	tmp.u.r = n;
	tmp.flags = MEM_Real;
	return sqlite3VdbeSerialPut(data, &tmp, GetSerialTypeNum(n));
}

int DataVarintLenNum(u64 number) {
	if ((number == 0) || (number == 1)) return 0;
	return sqlite3VarintLen(number);
}

int DataVarintLenNum(i64 number) {
	u64 tmp;
	memcpy(&tmp, &number, sizeof(u64));
	return DataVarintLenNum(tmp);
}

int DataVarintLenNum(double) {
	return 8;
}

int CalculateHeaderSize(size_t h) {
	int l1 = sqlite3VarintLen(h);
	int l2 = sqlite3VarintLen(h + l1);
	return l2 + h;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ T A R A N T O O L   C U R S O R ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

bool TarantoolCursor::make_btree_cell_from_tuple() {
	if (tpl == NULL) return false;
	size = 0;
	int cnt = box_tuple_field_count(tpl);
	MValue *fields = new MValue[cnt];
	int *serial_types = new int[cnt];
	for (int i = 0; i < cnt; ++i) {
		const char *tmp = box_tuple_field(tpl, i);
		fields[i] = MValue::FromMSGPuck(&tmp);
		if (fields[i].GetType() == -1) {
			delete[] fields;
			delete[] serial_types;
			return false;
		}
	}

	int header_size = 0;
	int data_size = 0;

	for (int i = 0; i < cnt; ++i) {
		MValue *val = fields + i;
		serial_types[i] = 0;
		switch(val->GetType()) {
			case MP_NIL: break;
			case MP_UINT: {
				serial_types[i] = GetSerialTypeNum(val->GetUint64());
				header_size += sqlite3VarintLen(serial_types[i]);
				data_size += DataVarintLenNum(val->GetUint64());
				break;
			}
			case MP_STR: {
				size_t len;
				val->GetStr(&len);
				serial_types[i] = GetSerialTypeStr(len);
				header_size += sqlite3VarintLen(serial_types[i]);
				data_size += len;
				break;
			}
			case MP_INT: {
				serial_types[i] = GetSerialTypeNum(val->GetInt64());
				header_size += sqlite3VarintLen(serial_types[i]);
				data_size += DataVarintLenNum(val->GetInt64());
				break;
			}
			case MP_BIN: {
				size_t len;
				val->GetBin(&len);
				serial_types[i] = GetSerialTypeStr(len);
				header_size += sqlite3VarintLen(serial_types[i]);
				data_size += len;
				break;
			}
			case MP_BOOL: {
				u64 tmp = (val->GetBool()) ? 1 : 0;
				serial_types[i] = GetSerialTypeNum(tmp);
				header_size += sqlite3VarintLen(serial_types[i]);
				data_size += DataVarintLenNum(tmp);
				break;
			}
			case MP_DOUBLE: {
				double tmp = val->GetDouble();
				serial_types[i] = GetSerialTypeNum(tmp);
				header_size += sqlite3VarintLen(serial_types[i]);
				data_size += DataVarintLenNum(tmp);
				break;
			}
			default: {
				delete[] fields;
				delete[] serial_types;
				return false;
			}
		}
	}

	header_size = CalculateHeaderSize(header_size);
	data = sqlite3DbMallocZero(db, header_size + data_size);
	int offset = 0;
	offset += sqlite3PutVarint((unsigned char *)data + offset, header_size);
	for (int i = 0; i < cnt; ++i) {
		offset += sqlite3PutVarint((unsigned char *)data + offset, serial_types[i]);
	}

	for (int i = 0; i < cnt; ++i) {
		MValue *val = fields + i;
		switch(val->GetType()) {
			case MP_UINT: {
				offset += PutVarintDataNum((unsigned char *)data + offset, val->GetUint64());
				break;
			}
			case MP_INT: {
				offset += PutVarintDataNum((unsigned char *)data + offset, val->GetInt64());
				break;
			}
			case MP_STR: {
				size_t len;
				const char *tmp = val->GetStr(&len);
				memcpy((char *)data + offset, tmp, len);
				offset += len;
				break;
			}
			case MP_BIN: {
				size_t len;
				const char *tmp = val->GetBin(&len);
				memcpy((char *)data + offset, tmp, len);
				offset += len;
				break;
			}
			case MP_BOOL: {
				u64 tmp = (val->GetBool()) ? 1 : 0;
				offset += PutVarintDataNum((unsigned char *)data + offset, tmp);
				break;
			}
			case MP_DOUBLE: {
				double tmp = val->GetDouble();
				offset += PutVarintDataNum((unsigned char *)data + offset, tmp);
				break;
			}
			default: break;
		}		
	}
	size = header_size + data_size;
	return true;
}

bool TarantoolCursor::make_btree_key_from_tuple() {
	if (sql_index == NULL) return false;
	if (tpl == NULL) return false;
	size = 0;
	int cnt = box_tuple_field_count(tpl);
	MValue *fields = new MValue[cnt];
	int *serial_types = new int[cnt];
	int *another_cols = new int[sql_index->nColumn - sql_index->nKeyCol];
	for (int i = 0, k = 0; i < sql_index->nColumn; ++i) {
		bool found = false;
		for (int j = 0; j < sql_index->nKeyCol; ++j) {
			if (i == sql_index->aiColumn[j]) {
				found = true;
				break;
			}
		}
		if (!found) another_cols[k++] = i;
	}
	for (int i = 0; i < sql_index->nKeyCol; ++i) {
		const char *tmp = box_tuple_field(tpl, sql_index->aiColumn[i]);
		fields[size] = MValue::FromMSGPuck(&tmp);
		if (fields[size].GetType() == -1) {
			delete[] fields;
			delete[] serial_types;
			return false;
		}
		++size;
	}
	for (int i = 0, cnt2 = sql_index->nColumn - sql_index->nKeyCol; i < cnt2; ++i) {
		const char *tmp = box_tuple_field(tpl, another_cols[i]);
		fields[size] = MValue::FromMSGPuck(&tmp);
		if (fields[size].GetType() == -1) {
			delete[] fields;
			delete[] serial_types;
			return false;
		}
		++size;
	}
	delete[] another_cols;
	size = 0;

	int header_size = 0;
	int data_size = 0;

	for (int i = 0; i < cnt; ++i) {
		MValue *val = fields + i;
		serial_types[i] = 0;
		switch(val->GetType()) {
			case MP_NIL: break;
			case MP_UINT: {
				serial_types[i] = GetSerialTypeNum(val->GetUint64());
				header_size += sqlite3VarintLen(serial_types[i]);
				data_size += DataVarintLenNum(val->GetUint64());
				break;
			}
			case MP_STR: {
				size_t len;
				val->GetStr(&len);
				serial_types[i] = GetSerialTypeStr(len);
				header_size += sqlite3VarintLen(serial_types[i]);
				data_size += len;
				break;
			}
			case MP_INT: {
				serial_types[i] = GetSerialTypeNum(val->GetInt64());
				header_size += sqlite3VarintLen(serial_types[i]);
				data_size += DataVarintLenNum(val->GetInt64());
				break;
			}
			case MP_BIN: {
				size_t len;
				val->GetBin(&len);
				serial_types[i] = GetSerialTypeStr(len);
				header_size += sqlite3VarintLen(serial_types[i]);
				data_size += len;
				break;
			}
			case MP_BOOL: {
				u64 tmp = (val->GetBool()) ? 1 : 0;
				serial_types[i] = GetSerialTypeNum(tmp);
				header_size += sqlite3VarintLen(serial_types[i]);
				data_size += DataVarintLenNum(tmp);
				break;
			}
			case MP_DOUBLE: {
				double tmp = val->GetDouble();
				serial_types[i] = GetSerialTypeNum(tmp);
				header_size += sqlite3VarintLen(serial_types[i]);
				data_size += DataVarintLenNum(tmp);
				break;
			}
			default: {
				delete[] fields;
				delete[] serial_types;
				return false;
			}
		}
	}

	header_size = CalculateHeaderSize(header_size);
	data = sqlite3DbMallocZero(db, malloc_size(header_size + data_size));
	int offset = 0;
	offset += sqlite3PutVarint((unsigned char *)data + offset, header_size);
	for (int i = 0; i < cnt; ++i) {
		offset += sqlite3PutVarint((unsigned char *)data + offset, serial_types[i]);
	}

	for (int i = 0; i < cnt; ++i) {
		MValue *val = fields + i;
		switch(val->GetType()) {
			case MP_UINT: {
				offset += PutVarintDataNum((unsigned char *)data + offset, val->GetUint64());
				break;
			}
			case MP_INT: {
				offset += PutVarintDataNum((unsigned char *)data + offset, val->GetInt64());
				break;
			}
			case MP_STR: {
				size_t len;
				const char *tmp = val->GetStr(&len);
				memcpy((char *)data + offset, tmp, len);
				offset += len;
				break;
			}
			case MP_BIN: {
				size_t len;
				const char *tmp = val->GetBin(&len);
				memcpy((char *)data + offset, tmp, len);
				offset += len;
				break;
			}
			case MP_BOOL: {
				u64 tmp = (val->GetBool()) ? 1 : 0;
				offset += PutVarintDataNum((unsigned char *)data + offset, tmp);
				break;
			}
			case MP_DOUBLE: {
				double tmp = val->GetDouble();
				offset += PutVarintDataNum((unsigned char *)data + offset, tmp);
				break;
			}
			default: break;
		}		
	}
	delete[] fields;
	delete[] serial_types;
	size = header_size + data_size;
	return true;
}

TarantoolCursor::TarantoolCursor() : space_id(0), index_id(0), type(-1), key(NULL),
	key_end(NULL), it(NULL), tpl(NULL), db(NULL), data(NULL), size(0) { }

TarantoolCursor::TarantoolCursor(sqlite3 *db_, uint32_t space_id_, uint32_t index_id_, int type_,
               const char *key_, const char *key_end_, SIndex *sql_index_)
: space_id(space_id_), index_id(index_id_), type(type_), key(key_), key_end(key_end_),
	tpl(NULL), sql_index(sql_index_), db(db_), data(NULL), size(0) {
	it = box_index_iterator(space_id, index_id, type, key, key_end);
}

int TarantoolCursor::MoveToFirst(int *pRes) {
	static const char *__func_name = "TarantoolCursor::MoveToFirst";

	it = box_index_iterator(space_id, index_id, type, key, key_end);
	int rc = box_iterator_next(it, &tpl);
	if (rc) {
		say_debug("%s(): box_iterator_next return rc = %d <> 0\n", __func_name, rc);
		*pRes = 1;
		tpl = NULL;
	} else {
		*pRes = 0;
	}
	if (tpl == NULL) {
		*pRes = 1;
		return SQLITE_OK;
	}
	rc = this->make_btree_cell_from_tuple();
	return SQLITE_OK;
}

int TarantoolCursor::DataSize(u32 *pSize) const {
	*pSize = size;
	return SQLITE_OK;
}

const void *TarantoolCursor::DataFetch(u32 *pAmt) const {
	*pAmt = size;
	return data;
}

int TarantoolCursor::KeySize(i64 *pSize) {
	this->make_btree_key_from_tuple();
	*pSize = size;
	return SQLITE_OK;
}

const void *TarantoolCursor::KeyFetch(u32 *pAmt) {
	this->make_btree_key_from_tuple();
	*pAmt = size;
	return data;
}

int TarantoolCursor::Next(int *pRes) {
	static const char *__func_name = "TarantoolCursor::Next";

	int rc = box_iterator_next(it, &tpl);
	if (rc) {
		say_debug("%s(): box_iterator_next return rc = %d <> 0\n", __func_name, rc);
		*pRes = 1;
		return SQLITE_OK;
	}
	if (!tpl) {
		*pRes = 1;
		return SQLITE_OK;
	} else {
		*pRes = 0;
	}
	rc = this->make_btree_cell_from_tuple();
	return SQLITE_OK;
}

int TarantoolCursor::MoveToUnpacked(UnpackedRecord *pIdxKey, i64 intKey, int *pRes, RecordCompare xRecordCompare) {
	static const char *__func_name = "TarantoolCursor::MoveToUnpacked";
	int rc = SQLITE_OK;
	if (!xRecordCompare) {
		say_debug("%s(): intKey not supported, intKey = %lld\n", __func_name, intKey);
		return SQLITE_ERROR;
	} else {
		rc = this->MoveToFirst(pRes);
		if (!tpl) {
			*pRes = -1;
			rc = SQLITE_OK;
			say_debug("%s(): space is empty\n", __func_name);
			return rc;
		}
		if (rc) {
			say_debug("%s(): MoveToFirst return rc = %d <> 0\n", __func_name, rc);
			return rc;
		}
		*pRes = 0;
		while(!*pRes) {
			i64 data_size;
			u32 tmp_;
			this->make_btree_key_from_tuple();
			if ((rc = this->KeySize(&data_size))) {
				say_debug("%s(): DataSize return rc = %d <> 0\n", __func_name, rc);
				return rc;
			}
			const void *data = this->KeyFetch(&tmp_);
			data_size = tmp_;
			int c = xRecordCompare(data_size, data, pIdxKey);
			if (!c) {
				*pRes = 0;
          		rc = SQLITE_OK;
          		say_debug("%s(): find match\n", __func_name);
          		return rc;
			}
			if (c > 0) {
				*pRes = 1;
				rc = SQLITE_OK;
				say_debug("%s(): no matches found\n", __func_name);
				return rc;
			}
			rc = this->Next(pRes);
			if (rc) {
				say_debug("%s(): Next return rc = %d <> 0\n", __func_name, rc);
				return rc;
			}
		}
		*pRes = -1;
		rc = SQLITE_OK;
	}
	return rc;
}

TarantoolCursor::TarantoolCursor(const TarantoolCursor &ob) : data(NULL), size(0) {
	*this = ob;
}

TarantoolCursor &TarantoolCursor::operator=(const TarantoolCursor &ob) {
	space_id = ob.space_id;
	index_id = ob.index_id;
	type = ob.type;
	key = ob.key;
	key_end = ob.key_end;
	tpl = ob.tpl;
	db = ob.db;
	sql_index = ob.sql_index;
	if (ob.size) {
		size = ob.size;
		data = sqlite3DbMallocZero(db, malloc_size(size));
		memcpy(data, ob.data, size);
	}
	return *this;
}

TarantoolCursor::~TarantoolCursor() {
	if (it) box_iterator_free(it);
	box_txn_commit();
}