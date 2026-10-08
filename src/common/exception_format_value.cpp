#include "duckdb/common/exception.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/string_format.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/uhugeint.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/common/types/string.hpp"
#include "duckdb/common/identifier.hpp"

namespace duckdb {

ExceptionFormatValue::ExceptionFormatValue(double dbl_val)
    : type(ExceptionFormatValueType::FORMAT_VALUE_TYPE_DOUBLE), dbl_val(dbl_val) {
}
ExceptionFormatValue::ExceptionFormatValue(int64_t int_val)
    : type(ExceptionFormatValueType::FORMAT_VALUE_TYPE_INTEGER), int_val(int_val) {
}
ExceptionFormatValue::ExceptionFormatValue(idx_t uint_val)
    : type(ExceptionFormatValueType::FORMAT_VALUE_TYPE_INTEGER), int_val(Hugeint::Convert(uint_val)) {
}
ExceptionFormatValue::ExceptionFormatValue(hugeint_t huge_val)
    : type(ExceptionFormatValueType::FORMAT_VALUE_TYPE_STRING), str_val(Hugeint::ToString(huge_val)) {
}
ExceptionFormatValue::ExceptionFormatValue(uhugeint_t uhuge_val)
    : type(ExceptionFormatValueType::FORMAT_VALUE_TYPE_STRING), str_val(Uhugeint::ToString(uhuge_val)) {
}
ExceptionFormatValue::ExceptionFormatValue(string str_val)
    : type(ExceptionFormatValueType::FORMAT_VALUE_TYPE_STRING), str_val(std::move(str_val)) {
}
ExceptionFormatValue::ExceptionFormatValue(const String &str_val) : ExceptionFormatValue(str_val.ToStdString()) {
}

template <>
ExceptionFormatValue ExceptionFormatValue::CreateFormatValue(const PhysicalType &value) {
	return ExceptionFormatValue(TypeIdToString(value));
}
template <>
ExceptionFormatValue ExceptionFormatValue::CreateFormatValue(const LogicalType &value) {
	return ExceptionFormatValue(value.ToString());
}
template <>
ExceptionFormatValue ExceptionFormatValue::CreateFormatValue(const float &value) {
	return ExceptionFormatValue(static_cast<double>(value));
}
template <>
ExceptionFormatValue ExceptionFormatValue::CreateFormatValue(const double &value) {
	return ExceptionFormatValue(value);
}
template <>
ExceptionFormatValue ExceptionFormatValue::CreateFormatValue(const string &value) {
	return ExceptionFormatValue(value);
}
template <>
ExceptionFormatValue ExceptionFormatValue::CreateFormatValue(const String &value) {
	return ExceptionFormatValue(value);
}
template <>
ExceptionFormatValue ExceptionFormatValue::CreateFormatValue(const Identifier &value) {
	return SQLQuotedIdentifier::ToString(value);
}

template <>
ExceptionFormatValue ExceptionFormatValue::CreateFormatValue(const SQLString &value) {
	return SQLString::ToString(value.raw_string);
}

template <>
ExceptionFormatValue ExceptionFormatValue::CreateFormatValue(const SQLIdentifier &value) {
	return SQLIdentifier::ToString(value.raw_string);
}

template <>
ExceptionFormatValue ExceptionFormatValue::CreateFormatValue(const char *const &value) {
	return ExceptionFormatValue(string(value));
}
template <>
ExceptionFormatValue ExceptionFormatValue::CreateFormatValue(char *const &value) {
	return ExceptionFormatValue(string(value));
}
template <>
ExceptionFormatValue ExceptionFormatValue::CreateFormatValue(const std::string_view &value) {
	return ExceptionFormatValue(string(value));
}
template <>
ExceptionFormatValue ExceptionFormatValue::CreateFormatValue(const idx_t &value) {
	return ExceptionFormatValue(value);
}
template <>
ExceptionFormatValue ExceptionFormatValue::CreateFormatValue(const hugeint_t &value) {
	return ExceptionFormatValue(value);
}
template <>
ExceptionFormatValue ExceptionFormatValue::CreateFormatValue(const uhugeint_t &value) {
	return ExceptionFormatValue(value);
}

string ExceptionFormatValue::Format(std::string_view msg, std::vector<ExceptionFormatValue> &values) {
	try {
		vector<FormatArgument> format_args;
		format_args.reserve(values.size());
		for (auto &val : values) {
			switch (val.type) {
			case ExceptionFormatValueType::FORMAT_VALUE_TYPE_DOUBLE:
				format_args.emplace_back(val.dbl_val);
				break;
			case ExceptionFormatValueType::FORMAT_VALUE_TYPE_INTEGER:
				format_args.emplace_back(val.int_val);
				break;
			case ExceptionFormatValueType::FORMAT_VALUE_TYPE_STRING:
				format_args.emplace_back(std::string_view(val.str_val));
				break;
			}
		}
		return StringFormat::Printf(msg, format_args);
	} catch (std::exception &ex) { // LCOV_EXCL_START
		// work-around for oss-fuzz limiting memory which causes issues here
		if (StringUtil::Contains(ex.what(), "fuzz mode")) {
			throw InvalidInputException(msg);
		}
		throw InternalException(
		    absl::StrCat("Primary exception: ", msg, "\nSecondary exception in ExceptionFormatValue: ", ex.what()));
	} // LCOV_EXCL_STOP
}

} // namespace duckdb
