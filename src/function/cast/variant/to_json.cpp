#include "duckdb/function/cast/variant/variant_json_writer.hpp"

#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/variant_visitor.hpp"

#include "simdutf.h"

namespace duckdb {

//! ------------ Variant -> JSON ------------

namespace {

template <bool TEXT>
struct JSONConverterBase {
	using result_type = void;

	static void WriteString(std::string_view text, JSONStringWriter &writer) {
		if constexpr (TEXT) {
			writer.Raw(text);
		} else {
			writer.String(text);
		}
	}

	static void VisitNull(JSONStringWriter &writer) {
		writer.Null();
	}

	static void VisitBoolean(bool val, JSONStringWriter &writer) {
		writer.Boolean(val);
	}

	template <typename T>
	static void VisitInteger(T val, JSONStringWriter &writer) {
		if constexpr (std::is_same_v<T, int8_t> || std::is_same_v<T, int16_t> || std::is_same_v<T, int32_t> ||
		              std::is_same_v<T, int64_t> || std::is_same_v<T, uint8_t> || std::is_same_v<T, uint16_t> ||
		              std::is_same_v<T, uint32_t>) {
			writer.SignedInteger(val);
		} else if constexpr (std::is_same_v<T, uint64_t>) {
			writer.UnsignedInteger(val);
		} else {
			writer.Raw(val.ToString());
		}
	}

	static void VisitTime(dtime_t val, JSONStringWriter &writer) {
		WriteString(Time::ToString(val), writer);
	}

	static void VisitTimeNanos(dtime_ns_t val, JSONStringWriter &writer) {
		WriteString(Value::TIME_NS(val).ToString(), writer);
	}

	static void VisitTimeTZ(dtime_tz_t val, JSONStringWriter &writer) {
		WriteString(Value::TIMETZ(val).ToString(), writer);
	}

	static void VisitTimestampSec(timestamp_sec_t val, JSONStringWriter &writer) {
		WriteString(Value::TIMESTAMPSEC(val).ToString(), writer);
	}

	static void VisitTimestampMs(timestamp_ms_t val, JSONStringWriter &writer) {
		WriteString(Value::TIMESTAMPMS(val).ToString(), writer);
	}

	static void VisitTimestamp(timestamp_t val, JSONStringWriter &writer) {
		WriteString(Timestamp::ToString(val), writer);
	}

	static void VisitTimestampNanos(timestamp_ns_t val, JSONStringWriter &writer) {
		WriteString(Value::TIMESTAMPNS(val).ToString(), writer);
	}

	static void VisitTimestampTZ(timestamp_tz_t val, JSONStringWriter &writer) {
		WriteString(Value::TIMESTAMPTZ(val).ToString(), writer);
	}

	static void VisitTimestampTZNanos(timestamp_tz_ns_t val, JSONStringWriter &writer) {
		WriteString(Value::TIMESTAMPTZNS(val).ToString(), writer);
	}

	static void VisitFloat(float val, JSONStringWriter &writer) {
		writer.Double(val);
	}

	static void VisitDouble(double val, JSONStringWriter &writer) {
		writer.Double(val);
	}

	static void VisitUUID(hugeint_t val, JSONStringWriter &writer) {
		WriteString(Value::UUID(val).ToString(), writer);
	}

	static void VisitDate(date_t val, JSONStringWriter &writer) {
		WriteString(Date::ToString(val), writer);
	}

	static void VisitInterval(interval_t val, JSONStringWriter &writer) {
		WriteString(Value::INTERVAL(val).ToString(), writer);
	}

	static void VisitString(const string_t &str, JSONStringWriter &writer) {
		WriteString(std::string_view(str.GetData(), str.GetSize()), writer);
	}

	static void VisitBlob(const string_t &str, JSONStringWriter &writer) {
		WriteString(Value::BLOB(const_data_ptr_cast(str.GetData()), str.GetSize()).ToString(), writer);
	}

	static void VisitBignum(const string_t &str, JSONStringWriter &writer) {
		writer.Raw(Value::BIGNUM(const_data_ptr_cast(str.GetData()), str.GetSize()).ToString());
	}

	static void VisitGeometry(const string_t &str, JSONStringWriter &writer) {
		WriteString(Value::GEOMETRY(const_data_ptr_cast(str.GetData()), str.GetSize()).ToString(), writer);
	}

	static void VisitBitstring(const string_t &str, JSONStringWriter &writer) {
		WriteString(Value::BIT(const_data_ptr_cast(str.GetData()), str.GetSize()).ToString(), writer);
	}

	template <typename T>
	static void VisitDecimal(T val, uint32_t width, uint32_t scale, JSONStringWriter &writer) {
		string val_str;
		if constexpr (std::is_same_v<T, int16_t>) {
			val_str = Decimal::ToString(val, static_cast<uint8_t>(width), static_cast<uint8_t>(scale));
		} else if constexpr (std::is_same_v<T, int32_t>) {
			val_str = Decimal::ToString(val, static_cast<uint8_t>(width), static_cast<uint8_t>(scale));
		} else if constexpr (std::is_same_v<T, int64_t>) {
			val_str = Decimal::ToString(val, static_cast<uint8_t>(width), static_cast<uint8_t>(scale));
		} else if constexpr (std::is_same_v<T, hugeint_t>) {
			val_str = Decimal::ToString(val, static_cast<uint8_t>(width), static_cast<uint8_t>(scale));
		} else {
			throw InternalException("Unhandled decimal type");
		}
		writer.Raw(val_str);
	}

	static void VisitDefault(VariantLogicalType type_id, const_data_ptr_t, JSONStringWriter &) {
		throw InternalException("VariantLogicalType(%s) not handled", EnumUtil::ToString(type_id));
	}
};

struct JSONConverter : JSONConverterBase<false> {
	static void VisitArray(const UnifiedVariantVectorData &variant, idx_t row, const VariantNestedData &nested_data,
	                       JSONStringWriter &writer) {
		writer.StartArray();
		VariantVisitor<JSONConverter>::VisitArrayItems(variant, row, nested_data, writer);
		writer.EndArray();
	}

	static void VisitObject(const UnifiedVariantVectorData &variant, idx_t row, const VariantNestedData &nested_data,
	                        JSONStringWriter &writer) {
		vector<std::pair<string_t, idx_t>> members;
		members.reserve(nested_data.child_count);
		for (idx_t i = 0; i < nested_data.child_count; i++) {
			auto keys_index = variant.GetKeysIndex(row, nested_data.children_idx + i);
			members.emplace_back(variant.GetKey(row, keys_index), nested_data.children_idx + i);
		}
		std::sort(members.begin(), members.end());

		writer.StartObject();
		for (idx_t i = 0; i < members.size(); i++) {
			auto &key = members[i].first;
			if (i + 1 < members.size() && members[i + 1].first == key) {
				continue;
			}
			writer.Key(std::string_view(key.GetData(), key.GetSize()));
			auto values_index = variant.GetValuesIndex(row, members[i].second);
			VariantVisitor<JSONConverter>::Visit(variant, row, values_index, writer);
		}
		writer.EndObject();
	}
};

template <bool TEXT>
struct NodeJSONConverter : JSONConverterBase<TEXT> {
	static void VisitArray(const VariantNode &variant, JSONStringWriter &writer) {
		writer.StartArray();
		VariantVisitor<NodeJSONConverter<false>>::VisitArrayItems(variant, writer);
		writer.EndArray();
	}

	static void VisitObject(const VariantNode &variant, JSONStringWriter &writer) {
		writer.StartObject();
		for (const auto &[key, value] : variant.GetObjectChildren(VariantIterationOrder::LEXICOGRAPHIC)) {
			writer.Key(std::string_view(key.GetData(), key.GetSize()));
			VariantVisitor<NodeJSONConverter<false>>::Visit(value, writer);
		}
		writer.EndObject();
	}
};

} // namespace

VariantJSONWriter::VariantJSONWriter()
    : writer(JSONWriteFlags::ALLOW_INF_AND_NAN | JSONWriteFlags::ALLOW_INVALID_UNICODE) {
}

optional<string_t> VariantJSONWriter::Write(const UnifiedVariantVectorData &variant, idx_t row, uint32_t values_idx,
                                            string &error) {
	writer.Clear();
	VariantVisitor<JSONConverter>::Visit(variant, row, values_idx, writer);
	return Result(error);
}

optional<string_t> VariantJSONWriter::WriteText(const VariantNode &variant, string &error) {
	writer.Clear();
	VariantVisitor<NodeJSONConverter<true>>::Visit(variant, writer);
	return Result(error);
}

optional<string_t> VariantJSONWriter::Result(string &error) const {
	const auto json = writer.View();
	if (!simdutf::validate_utf8(json.data(), json.size())) {
		error = "Failed to serialize JSON: invalid utf-8 encoding in string";
		return {};
	}
	return string_t(json.data(), NumericCast<uint32_t>(json.size()));
}

} // namespace duckdb
