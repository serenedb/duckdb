//===----------------------------------------------------------------------===//
//                         DuckDB
//
// thrift_buffer_protocol.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "thrift/protocol/TProtocolException.h"
#include "thrift/protocol/TVirtualProtocol.h"
#include "thrift/transport/TTransportException.h"

#include "duckdb/common/assert.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/serializer/encoding_util.hpp"
#include "duckdb/common/typedefs.hpp"

#include <cstring>
#include <string>

namespace duckdb {

//! Reads thrift's compact protocol from bytes that are in memory, such as a parquet footer read in one piece. It
//! decodes the bytes where they lie, while TCompactProtocolT asks its transport for every byte
class ThriftBufferProtocol final : public duckdb_apache::thrift::protocol::TVirtualProtocol<ThriftBufferProtocol> {
	using TType = duckdb_apache::thrift::protocol::TType;
	using TProtocolException = duckdb_apache::thrift::protocol::TProtocolException;

public:
	ThriftBufferProtocol(const_data_ptr_t data, idx_t size) : TVirtualProtocol(nullptr), pos(data), end(data + size) {
	}

	uint32_t readStructBegin(std::string &) {
		D_ASSERT(depth < MAX_DEPTH);
		last_field_ids[depth++] = last_field_id;
		last_field_id = 0;
		return 0;
	}

	uint32_t readStructEnd() {
		last_field_id = last_field_ids[--depth];
		return 0;
	}

	uint32_t readFieldBegin(std::string &, TType &field_type, int16_t &field_id) {
		const auto start = pos;
		const auto header = ReadByte();
		const uint8_t type = header & 0x0f;
		if (type == CT_STOP) {
			field_type = duckdb_apache::thrift::protocol::T_STOP;
			field_id = 0;
			return 1;
		}
		const auto delta = UnsafeNumericCast<int16_t>(header >> 4);
		if (delta == 0) {
			field_id = static_cast<int16_t>(EncodingUtil::ZigZagDecode(static_cast<uint32_t>(ReadVarint())));
		} else {
			field_id = static_cast<int16_t>(last_field_id + delta);
		}
		field_type = ToTType(type);
		if (type == CT_BOOLEAN_TRUE || type == CT_BOOLEAN_FALSE) {
			// a boolean field holds its value in its header
			has_bool_value = true;
			bool_value = type == CT_BOOLEAN_TRUE;
		}
		last_field_id = field_id;
		return Read(start);
	}

	uint32_t readFieldEnd() {
		return 0;
	}

	uint32_t readMapBegin(TType &key_type, TType &value_type, uint32_t &size) {
		const auto start = pos;
		const auto map_size = static_cast<int32_t>(ReadVarint());
		const uint8_t types = map_size == 0 ? 0 : ReadByte();
		size = CheckSize(map_size);
		key_type = ToTType(types >> 4);
		value_type = ToTType(types & 0x0f);
		return Read(start);
	}

	uint32_t readMapEnd() {
		return 0;
	}

	uint32_t readListBegin(TType &element_type, uint32_t &size) {
		const auto start = pos;
		const auto header = ReadByte();
		auto list_size = UnsafeNumericCast<int32_t>(header >> 4);
		if (list_size == 15) {
			list_size = static_cast<int32_t>(ReadVarint());
		}
		size = CheckSize(list_size);
		element_type = ToTType(header & 0x0f);
		return Read(start);
	}

	uint32_t readListEnd() {
		return 0;
	}

	uint32_t readSetBegin(TType &element_type, uint32_t &size) {
		return readListBegin(element_type, size);
	}

	uint32_t readSetEnd() {
		return 0;
	}

	uint32_t readBool(bool &value) {
		if (has_bool_value) {
			value = bool_value;
			has_bool_value = false;
			return 0;
		}
		value = ReadByte() == CT_BOOLEAN_TRUE;
		return 1;
	}
	using TVirtualProtocol::readBool;

	uint32_t readByte(int8_t &byte) {
		byte = static_cast<int8_t>(ReadByte());
		return 1;
	}

	uint32_t readI16(int16_t &value) {
		const auto start = pos;
		value = static_cast<int16_t>(EncodingUtil::ZigZagDecode(static_cast<uint32_t>(ReadVarint())));
		return Read(start);
	}

	uint32_t readI32(int32_t &value) {
		const auto start = pos;
		value = EncodingUtil::ZigZagDecode(static_cast<uint32_t>(ReadVarint()));
		return Read(start);
	}

	uint32_t readI64(int64_t &value) {
		const auto start = pos;
		value = EncodingUtil::ZigZagDecode(ReadVarint());
		return Read(start);
	}

	uint32_t readDouble(double &value) {
		Require(sizeof(double));
		uint64_t bits;
		memcpy(&bits, pos, sizeof(bits));
		pos += sizeof(bits);
		value = duckdb_apache::thrift::bitwise_cast<double>(THRIFT_letohll(bits));
		return sizeof(bits);
	}

	uint32_t readString(std::string &value) {
		return readBinary(value);
	}

	uint32_t readBinary(std::string &value) {
		const auto start = pos;
		const auto size = static_cast<int32_t>(ReadVarint());
		if (size < 0) {
			throw TProtocolException(TProtocolException::NEGATIVE_SIZE);
		}
		Require(UnsafeNumericCast<idx_t>(size));
		value.assign(const_char_ptr_cast(pos), UnsafeNumericCast<size_t>(size));
		pos += size;
		return Read(start);
	}

private:
	//! The types of the compact protocol, as they are written in the field headers
	enum CompactType : uint8_t {
		CT_STOP = 0x00,
		CT_BOOLEAN_TRUE = 0x01,
		CT_BOOLEAN_FALSE = 0x02,
		CT_BYTE = 0x03,
		CT_I16 = 0x04,
		CT_I32 = 0x05,
		CT_I64 = 0x06,
		CT_DOUBLE = 0x07,
		CT_BINARY = 0x08,
		CT_LIST = 0x09,
		CT_SET = 0x0A,
		CT_MAP = 0x0B,
		CT_STRUCT = 0x0C
	};
	//! A struct is read only within the recursion limit of the protocol (TInputRecursionTracker), so the field ids of
	//! the enclosing structs are kept for as many
	static constexpr idx_t MAX_DEPTH = duckdb_apache::thrift::protocol::DEFAULT_RECURSION_LIMIT;

	[[noreturn]] [[gnu::noinline]] [[gnu::cold]] static void ThrowUnknownType(uint8_t type) {
		throw duckdb_apache::thrift::TException(std::string("don't know what type: ") +
		                                        std::to_string(static_cast<int>(type)));
	}

	[[noreturn]] [[gnu::noinline]] [[gnu::cold]] static void ThrowEndOfData() {
		throw duckdb_apache::thrift::transport::TTransportException(
		    duckdb_apache::thrift::transport::TTransportException::END_OF_FILE, "No more data to read.");
	}

	[[noreturn]] [[gnu::noinline]] [[gnu::cold]] static void ThrowVarintTooLong() {
		throw TProtocolException(TProtocolException::INVALID_DATA, "Variable-length int over 10 bytes.");
	}

	[[gnu::always_inline]] static TType ToTType(uint8_t type) {
		using namespace duckdb_apache::thrift::protocol;
		static constexpr TType TYPES[] = {T_STOP,   T_BOOL,   T_BOOL, T_BYTE, T_I16, T_I32,   T_I64,
		                                  T_DOUBLE, T_STRING, T_LIST, T_SET,  T_MAP, T_STRUCT};
		if (type > CT_STRUCT) {
			ThrowUnknownType(type);
		}
		return TYPES[type];
	}

	[[gnu::always_inline]] uint32_t Read(const_data_ptr_t start) const {
		return UnsafeNumericCast<uint32_t>(pos - start);
	}

	[[gnu::always_inline]] idx_t Remaining() const {
		return UnsafeNumericCast<idx_t>(end - pos);
	}

	//! Containers hold at least a byte for each of their elements - a larger size is not allocated for
	[[gnu::always_inline]] uint32_t CheckSize(int32_t size) const {
		if (size < 0) {
			throw TProtocolException(TProtocolException::NEGATIVE_SIZE);
		}
		if (UnsafeNumericCast<idx_t>(size) > Remaining()) {
			throw TProtocolException(TProtocolException::SIZE_LIMIT);
		}
		return UnsafeNumericCast<uint32_t>(size);
	}

	[[gnu::always_inline]] void Require(idx_t size) const {
		if (size > Remaining()) {
			ThrowEndOfData();
		}
	}

	[[gnu::always_inline]] uint8_t ReadByte() {
		Require(1);
		return *pos++;
	}

	[[gnu::always_inline]] uint64_t ReadVarint() {
		Require(1);
		if (!(*pos & 0x80)) {
			return *pos++;
		}
		uint64_t result;
		const auto size = EncodingUtil::TryDecodeUnsignedLEB128(pos, Remaining(), result);
		if (size == 0) {
			if (Remaining() < EncodingUtil::MaxLEB128Size<uint64_t>()) {
				ThrowEndOfData();
			}
			ThrowVarintTooLong();
		}
		pos += size;
		return result;
	}

	const_data_ptr_t pos;
	const_data_ptr_t end;
	int16_t last_field_id = 0;
	idx_t depth = 0;
	int16_t last_field_ids[MAX_DEPTH];
	bool has_bool_value = false;
	bool bool_value = false;
};

} // namespace duckdb
