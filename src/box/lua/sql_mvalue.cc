#include "sql_mvalue.h"

MValue::MValue() : type(-1), data(NULL), data_len(0), error(false) { }

MValue::MValue(const MValue &ob) : type(-1), data(NULL), data_len(0), error(false) {
	*this = ob;
}

MValue &MValue::operator=(const MValue &ob) {
	Clear();
	type = ob.type;
	data_len = ob.data_len;
	error = ob.error;
	if (type == MP_ARRAY) {
		data = malloc(sizeof(MValue) * data_len);
		memset(data, 0, sizeof(MValue) * data_len);
		for (int i = 0; i < data_len; ++i) {
			*((MValue *)data + i) = *ob[i];
		}
	} else {
		data = malloc(data_len);
		memcpy(data, ob.data, data_len);
	}
	return *this;
}

double MValue::GetDouble() const {
	error = false;
	if (type != MP_DOUBLE) {
		error = true;
		return 0;
	}
	double res;
	memcpy(&res, data, sizeof(double));
	return res;
}

uint64_t MValue::GetUint64() const {
	error = false;
	if (type != MP_UINT) {
		error = true;
		return 0;
	}
	uint64_t res;
	memcpy(&res, data, sizeof(uint64_t));
	return res;
}

int64_t MValue::GetInt64() const {
	error = false;
	if (type != MP_INT) {
		error = true;
		return 0;
	}
	int64_t res;
	memcpy(&res, data, sizeof(int64_t));
	return res;
}

int MValue::Size() const {
	return data_len;
}

const char *MValue::GetStr(size_t *len) const {
	error = false;
	if (type != MP_STR) {
		error = true;
		return NULL;
	}
	if (len) {
		*len = data_len;
	}
	return (const char *)(data);
}

const char *MValue::GetBin(size_t *len) const {
	error = false;
	if (type != MP_BIN) {
		error = true;
		return NULL;
	}
	if (len) *len = data_len;
	return (const char *)data;
}

bool MValue::GetBool() const {
	error = false;
	if (type != MP_BOOL) {
		error = true;
		return false;
	}
	return !!(*((uint8_t *)data));
}

bool MValue::IsError() const { return error; }

bool MValue::IsEmpty() const {
	return ((!data) || (!data_len));
}

int MValue::GetType() const { return type; }

MValue *MValue::operator[](int index) {
	error = false;
	if ((type != MP_ARRAY) || (index >= data_len)) {
		error = true;
		return NULL;
	}
	return ((MValue *)(data)) + index;
}

const MValue *MValue::operator[](int index) const {
	error = false;
	if ((type != MP_ARRAY) || (index >= data_len)) {
		error = true;
		return NULL;
	}
	return ((const MValue *)(data)) + index;
}

MValue::~MValue() {
	Clear();
}

void MValue::Clear() {
	error = false;
	switch(type) {
		case MP_UINT: case MP_INT: case MP_STR: case MP_BIN: case MP_BOOL:
		case MP_FLOAT: case MP_DOUBLE: if (data) free(data); data = NULL; data_len = 0; break;
		case MP_ARRAY: {
			for (int i = 0; i < data_len; ++i) {
				((MValue *)(data) + i)->Clear();
			}
			if (data) {
				free(data);
				data = NULL;
				data_len = 0;
			}
			break;
		}
		default: return;
	}
}

MValue MValue::FromMSGPuck(const char **data) {
	MValue res;
	MValue::do_mvalue_from_msgpuck(&res, data);
	return res;
}

int MValue::do_mvalue_from_msgpuck(MValue *res, const char **data) {
	const char *tuple = *data;
	mp_type type = mp_typeof(*tuple);
	switch(type) {
		case MP_NIL:
			mp_decode_nil(&tuple);
			*data = tuple;
			res->type = type;
			res->data = NULL;
			res->data_len = 0;
			return 0;
		case MP_UINT: {
			uint64_t tmp = mp_decode_uint(&tuple);
			*data = tuple;
			res->type = type;
			res->data_len = sizeof(uint64_t);
			res->data = malloc(res->data_len);
			memcpy(res->data, &tmp, res->data_len);
			return 0;
		}
		case MP_INT: {
			int64_t tmp = mp_decode_int(&tuple);
			*data = tuple;
			res->type = type;
			res->data_len = sizeof(int64_t);
			res->data = malloc(res->data_len);
			memcpy(res->data, &tmp, res->data_len);
			return 0;
		}
		case MP_STR: {
			uint32_t len;
			const char *tmp = mp_decode_str(&tuple, &len);
			*data = tuple;
			res->type = type;
			res->data_len = len;
			res->data = malloc((res->data_len + 1) * sizeof(char));
			memcpy(res->data, tmp, res->data_len * sizeof(char));
			((char *)(res->data))[res->data_len] = 0;
			return 0;
		}
		case MP_BIN: {
			uint32_t len;
			const char *tmp = mp_decode_bin(&tuple, &len);
			*data = tuple;
			res->type = type;
			res->data_len = len;
			res->data = malloc(res->data_len * sizeof(char));
			memcpy(res->data, tmp, res->data_len);
			return 0;
		}
		case MP_BOOL: {
			uint8_t tmp = mp_decode_bool(&tuple);
			*data = tuple;
			res->type = type;
			res->data_len = sizeof(uint8_t);
			res->data = malloc(sizeof(uint8_t));
			memcpy(res->data, &tmp, res->data_len);
			return 0;
		}
		case MP_FLOAT: {
			float tmp = mp_decode_float(&tuple);
			*data = tuple;
			res->type = type;
			res->data_len = sizeof(float);
			res->data = malloc(res->data_len);
			memcpy(res->data, &tmp, res->data_len);
			return 0;
		}
		case MP_DOUBLE: {
			double tmp = mp_decode_double(&tuple);
			*data = tuple;
			res->type = type;
			res->data_len = sizeof(double);
			res->data = malloc(res->data_len);
			memcpy(res->data, &tmp, res->data_len);
			return 0;
		}
		case MP_ARRAY: {
			uint32_t size = mp_decode_array(&tuple);
			res->type = type;
			res->data_len = size;
			MValue *tmp = (MValue *)malloc(sizeof(MValue) * size);
			memset(tmp, 0, sizeof(MValue) * size);
			res->data = tmp;
			for (uint32_t i = 0; i < size; ++i) {
				MValue::do_mvalue_from_msgpuck(tmp + i, &tuple);
			}
			*data = tuple;
			return 0;
		}
		default: return -1;
	}
}

// uint32_t get_hash_mvalue(mvalue *) {
// 	static uint32_t hash_cntr = 0;
// 	return hash_cntr++;
// }