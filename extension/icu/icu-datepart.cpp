#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "include/icu-datepart.hpp"
#include "include/icu-datefunc.hpp"
#include "include/icu-zone-lut.hpp"

#include "duckdb/common/operator/date_trunc_operators.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/common/enums/date_part_specifier.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/common/vector_operations/binary_executor.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

namespace duckdb {

struct ICUDatePart : public ICUDateFunc {
	typedef int64_t (*part_bigint_t)(icu::Calendar *calendar, const uint64_t micros);
	typedef double (*part_double_t)(icu::Calendar *calendar, const uint64_t micros);

	// Date part adapters
	static int64_t ExtractEra(icu::Calendar *calendar, const uint64_t micros) {
		return ExtractField(calendar, UCAL_ERA);
	}

	static int64_t ExtractYear(icu::Calendar *calendar, const uint64_t micros) {
		return ExtractField(calendar, UCAL_YEAR);
	}

	static int64_t ExtractDecade(icu::Calendar *calendar, const uint64_t micros) {
		return ExtractYear(calendar, micros) / 10;
	}

	static int64_t ExtractCentury(icu::Calendar *calendar, const uint64_t micros) {
		const auto era = ExtractEra(calendar, micros);
		const auto cccc = ((ExtractYear(calendar, micros) - 1) / 100) + 1;
		return era > 0 ? cccc : -cccc;
	}

	static int64_t ExtractMillenium(icu::Calendar *calendar, const uint64_t micros) {
		const auto era = ExtractEra(calendar, micros);
		const auto mmmm = ((ExtractYear(calendar, micros) - 1) / 1000) + 1;
		return era > 0 ? mmmm : -mmmm;
	}

	static int64_t ExtractMonth(icu::Calendar *calendar, const uint64_t micros) {
		return ExtractField(calendar, UCAL_MONTH) + 1;
	}

	static int64_t ExtractQuarter(icu::Calendar *calendar, const uint64_t micros) {
		return ExtractField(calendar, UCAL_MONTH) / Interval::MONTHS_PER_QUARTER + 1;
	}

	static int64_t ExtractDay(icu::Calendar *calendar, const uint64_t micros) {
		return ExtractField(calendar, UCAL_DATE);
	}

	static int64_t ExtractDayOfWeek(icu::Calendar *calendar, const uint64_t micros) {
		// [Sun(0), Sat(6)]
		return ExtractField(calendar, UCAL_DAY_OF_WEEK) - UCAL_SUNDAY;
	}

	static int64_t ExtractISODayOfWeek(icu::Calendar *calendar, const uint64_t micros) {
		// [Mon(1), Sun(7)]
		return 1 + (ExtractField(calendar, UCAL_DAY_OF_WEEK) + 7 - UCAL_MONDAY) % 7;
	}

	static int64_t ExtractWeek(icu::Calendar *calendar, const uint64_t micros) {
		calendar->setFirstDayOfWeek(UCAL_MONDAY);
		calendar->setMinimalDaysInFirstWeek(4);
		return ExtractField(calendar, UCAL_WEEK_OF_YEAR);
	}

	static int64_t ExtractISOYear(icu::Calendar *calendar, const uint64_t micros) {
		calendar->setFirstDayOfWeek(UCAL_MONDAY);
		calendar->setMinimalDaysInFirstWeek(4);
		return ExtractField(calendar, UCAL_YEAR_WOY);
	}

	static int64_t ExtractYearWeek(icu::Calendar *calendar, const uint64_t micros) {
		calendar->setFirstDayOfWeek(UCAL_MONDAY);
		calendar->setMinimalDaysInFirstWeek(4);
		const auto iyyy = ExtractField(calendar, UCAL_YEAR_WOY);
		const auto ww = ExtractField(calendar, UCAL_WEEK_OF_YEAR);
		return iyyy * 100 + ((iyyy > 0) ? ww : -ww);
	}

	static int64_t ExtractDayOfYear(icu::Calendar *calendar, const uint64_t micros) {
		return ExtractField(calendar, UCAL_DAY_OF_YEAR);
	}

	static int64_t ExtractHour(icu::Calendar *calendar, const uint64_t micros) {
		return ExtractField(calendar, UCAL_HOUR_OF_DAY);
	}

	static int64_t ExtractMinute(icu::Calendar *calendar, const uint64_t micros) {
		return ExtractField(calendar, UCAL_MINUTE);
	}

	static int64_t ExtractSecond(icu::Calendar *calendar, const uint64_t micros) {
		return ExtractField(calendar, UCAL_SECOND);
	}

	static int64_t ExtractMillisecond(icu::Calendar *calendar, const uint64_t micros) {
		return ExtractSecond(calendar, micros) * Interval::MSECS_PER_SEC + ExtractField(calendar, UCAL_MILLISECOND);
	}

	static int64_t ExtractMicrosecond(icu::Calendar *calendar, const uint64_t micros) {
		return ExtractMillisecond(calendar, micros) * Interval::MICROS_PER_MSEC + micros;
	}

	static double ExtractEpoch(icu::Calendar *calendar, const uint64_t micros) {
		UErrorCode status = U_ZERO_ERROR;
		auto result = calendar->getTime(status) / Interval::MSECS_PER_SEC;
		result += micros / double(Interval::MICROS_PER_SEC);
		return result;
	}

	static int64_t ExtractTimezone(icu::Calendar *calendar, const uint64_t micros) {
		auto millis = ExtractField(calendar, UCAL_ZONE_OFFSET);
		millis += ExtractField(calendar, UCAL_DST_OFFSET);
		return millis / Interval::MSECS_PER_SEC;
	}

	static int64_t ExtractTimezoneHour(icu::Calendar *calendar, const uint64_t micros) {
		auto secs = ExtractTimezone(calendar, micros);
		return secs / Interval::SECS_PER_HOUR;
	}

	static int64_t ExtractTimezoneMinute(icu::Calendar *calendar, const uint64_t micros) {
		auto secs = ExtractTimezone(calendar, micros);
		return (secs % Interval::SECS_PER_HOUR) / Interval::SECS_PER_MINUTE;
	}

	//	PG uses doubles for JDs so we can only use them with other double types
	static double ExtractJulianDay(icu::Calendar *calendar, const uint64_t micros) {
		//	We need days + fraction
		auto days = ExtractField(calendar, UCAL_JULIAN_DAY);
		auto frac = ExtractHour(calendar, micros);

		frac *= Interval::MINS_PER_HOUR;
		frac += ExtractMinute(calendar, micros);

		frac *= Interval::MICROS_PER_MINUTE;
		frac += ExtractMicrosecond(calendar, micros);

		double result = frac;
		result /= Interval::MICROS_PER_DAY;
		result += days;

		return result;
	}

	struct LocalTime {
		timestamp_t wall;
		int64_t offset;
	};
	typedef int64_t (*local_bigint_t)(const LocalTime &local);
	typedef double (*local_double_t)(const LocalTime &local);

	[[gnu::always_inline]] static inline bool TryLocalTime(const ZoneLUT &lut, timestamp_tz_t input, LocalTime &local) {
		if (!lut.TryOffset(input.value, local.offset)) {
			return false;
		}
		local.wall = timestamp_t(input.value + local.offset);
		return local.wall.value >= ZoneLUT::FIRST_ANNO_DOMINI;
	}

	static inline int32_t LocalDays(const LocalTime &local) {
		return DateTrunc::ToDays(local.wall);
	}

	static inline int64_t LocalTimeOfDay(const LocalTime &local) {
		return local.wall.value - int64_t(LocalDays(local)) * Interval::MICROS_PER_DAY;
	}

	static inline DateTrunc::YearDay LocalYearDay(const LocalTime &local) {
		return DateTrunc::ToYearDay(LocalDays(local));
	}

	static int64_t LocalEra(const LocalTime &local) {
		return 1;
	}

	static int64_t LocalYear(const LocalTime &local) {
		return LocalYearDay(local).year;
	}

	static int64_t LocalDecade(const LocalTime &local) {
		return LocalYear(local) / 10;
	}

	static int64_t LocalCentury(const LocalTime &local) {
		return ((LocalYear(local) - 1) / 100) + 1;
	}

	static int64_t LocalMillenium(const LocalTime &local) {
		return ((LocalYear(local) - 1) / 1000) + 1;
	}

	static int64_t LocalMonth(const LocalTime &local) {
		return DateTrunc::MonthOf(LocalYearDay(local));
	}

	static int64_t LocalQuarter(const LocalTime &local) {
		return (LocalMonth(local) - 1) / Interval::MONTHS_PER_QUARTER + 1;
	}

	static int64_t LocalDay(const LocalTime &local) {
		const auto yd = LocalYearDay(local);
		return yd.doy - (DateTrunc::MonthStart(yd, DateTrunc::MonthOf(yd)) - yd.year_start) + 1;
	}

	static int64_t LocalISODayOfWeek(const LocalTime &local) {
		return Date::ExtractISODayOfTheWeek(date_t(LocalDays(local)));
	}

	static int64_t LocalDayOfWeek(const LocalTime &local) {
		return LocalISODayOfWeek(local) % 7;
	}

	static int64_t LocalWeek(const LocalTime &local) {
		return Date::ExtractISOWeekNumber(date_t(LocalDays(local)));
	}

	static int64_t LocalISOYear(const LocalTime &local) {
		return Date::ExtractISOYearNumber(date_t(LocalDays(local)));
	}

	static int64_t LocalYearWeek(const LocalTime &local) {
		const auto iyyy = LocalISOYear(local);
		const auto ww = LocalWeek(local);
		return iyyy * 100 + ((iyyy > 0) ? ww : -ww);
	}

	static int64_t LocalDayOfYear(const LocalTime &local) {
		return LocalYearDay(local).doy + 1;
	}

	static int64_t LocalHour(const LocalTime &local) {
		return LocalTimeOfDay(local) / Interval::MICROS_PER_HOUR;
	}

	static int64_t LocalMinute(const LocalTime &local) {
		return (LocalTimeOfDay(local) % Interval::MICROS_PER_HOUR) / Interval::MICROS_PER_MINUTE;
	}

	static int64_t LocalSecond(const LocalTime &local) {
		return (LocalTimeOfDay(local) % Interval::MICROS_PER_MINUTE) / Interval::MICROS_PER_SEC;
	}

	static int64_t LocalMillisecond(const LocalTime &local) {
		return (LocalTimeOfDay(local) % Interval::MICROS_PER_MINUTE) / Interval::MICROS_PER_MSEC;
	}

	static int64_t LocalMicrosecond(const LocalTime &local) {
		return LocalTimeOfDay(local) % Interval::MICROS_PER_MINUTE;
	}

	static int64_t LocalTimezone(const LocalTime &local) {
		return local.offset / Interval::MICROS_PER_SEC;
	}

	static int64_t LocalTimezoneHour(const LocalTime &local) {
		return LocalTimezone(local) / Interval::SECS_PER_HOUR;
	}

	static int64_t LocalTimezoneMinute(const LocalTime &local) {
		return (LocalTimezone(local) % Interval::SECS_PER_HOUR) / Interval::SECS_PER_MINUTE;
	}

	static double LocalEpoch(const LocalTime &local) {
		const int64_t instant = local.wall.value - local.offset;
		const int64_t millis = DateTrunc::FloorDiv(instant, Interval::MICROS_PER_MSEC);
		const uint64_t micros = UnsafeNumericCast<uint64_t>(instant - millis * Interval::MICROS_PER_MSEC);
		auto result = double(millis) / Interval::MSECS_PER_SEC;
		result += micros / double(Interval::MICROS_PER_SEC);
		return result;
	}

	static double LocalJulianDay(const LocalTime &local) {
		double result = double(LocalTimeOfDay(local));
		result /= Interval::MICROS_PER_DAY;
		result += double(Date::ExtractJulianDay(date_t(LocalDays(local))));
		return result;
	}

	static date_t LocalLastDay(const LocalTime &local) {
		const auto yd = LocalYearDay(local);
		const auto month = DateTrunc::MonthOf(yd);
		const auto days = yd.leap ? Date::LEAP_DAYS[month] : Date::NORMAL_DAYS[month];
		return date_t(UnsafeNumericCast<int32_t>(DateTrunc::MonthStart(yd, month) + days - 1));
	}

	static string_t LocalMonthName(const LocalTime &local) {
		return Date::MONTH_NAMES[LocalMonth(local) - 1];
	}

	static string_t LocalDayName(const LocalTime &local) {
		return Date::DAY_NAMES[LocalDayOfWeek(local)];
	}

	static local_bigint_t LocalBigintFactory(DatePartSpecifier part) {
		switch (part) {
		case DatePartSpecifier::YEAR:
			return LocalYear;
		case DatePartSpecifier::MONTH:
			return LocalMonth;
		case DatePartSpecifier::DAY:
			return LocalDay;
		case DatePartSpecifier::DECADE:
			return LocalDecade;
		case DatePartSpecifier::CENTURY:
			return LocalCentury;
		case DatePartSpecifier::MILLENNIUM:
			return LocalMillenium;
		case DatePartSpecifier::MICROSECONDS:
			return LocalMicrosecond;
		case DatePartSpecifier::MILLISECONDS:
			return LocalMillisecond;
		case DatePartSpecifier::SECOND:
			return LocalSecond;
		case DatePartSpecifier::MINUTE:
			return LocalMinute;
		case DatePartSpecifier::HOUR:
			return LocalHour;
		case DatePartSpecifier::DOW:
			return LocalDayOfWeek;
		case DatePartSpecifier::ISODOW:
			return LocalISODayOfWeek;
		case DatePartSpecifier::WEEK:
			return LocalWeek;
		case DatePartSpecifier::ISOYEAR:
			return LocalISOYear;
		case DatePartSpecifier::DOY:
			return LocalDayOfYear;
		case DatePartSpecifier::QUARTER:
			return LocalQuarter;
		case DatePartSpecifier::YEARWEEK:
			return LocalYearWeek;
		case DatePartSpecifier::ERA:
			return LocalEra;
		case DatePartSpecifier::TIMEZONE:
			return LocalTimezone;
		case DatePartSpecifier::TIMEZONE_HOUR:
			return LocalTimezoneHour;
		case DatePartSpecifier::TIMEZONE_MINUTE:
			return LocalTimezoneMinute;
		default:
			return nullptr;
		}
	}

	static local_double_t LocalDoubleFactory(DatePartSpecifier part) {
		switch (part) {
		case DatePartSpecifier::EPOCH:
			return LocalEpoch;
		case DatePartSpecifier::JULIAN_DAY:
			return LocalJulianDay;
		default:
			return nullptr;
		}
	}

	static part_bigint_t PartCodeBigintFactory(DatePartSpecifier part) {
		switch (part) {
		case DatePartSpecifier::YEAR:
			return ExtractYear;
		case DatePartSpecifier::MONTH:
			return ExtractMonth;
		case DatePartSpecifier::DAY:
			return ExtractDay;
		case DatePartSpecifier::DECADE:
			return ExtractDecade;
		case DatePartSpecifier::CENTURY:
			return ExtractCentury;
		case DatePartSpecifier::MILLENNIUM:
			return ExtractMillenium;
		case DatePartSpecifier::MICROSECONDS:
			return ExtractMicrosecond;
		case DatePartSpecifier::MILLISECONDS:
			return ExtractMillisecond;
		case DatePartSpecifier::SECOND:
			return ExtractSecond;
		case DatePartSpecifier::MINUTE:
			return ExtractMinute;
		case DatePartSpecifier::HOUR:
			return ExtractHour;
		case DatePartSpecifier::DOW:
			return ExtractDayOfWeek;
		case DatePartSpecifier::ISODOW:
			return ExtractISODayOfWeek;
		case DatePartSpecifier::WEEK:
			return ExtractWeek;
		case DatePartSpecifier::ISOYEAR:
			return ExtractISOYear;
		case DatePartSpecifier::DOY:
			return ExtractDayOfYear;
		case DatePartSpecifier::QUARTER:
			return ExtractQuarter;
		case DatePartSpecifier::YEARWEEK:
			return ExtractYearWeek;
		case DatePartSpecifier::ERA:
			return ExtractEra;
		case DatePartSpecifier::TIMEZONE:
			return ExtractTimezone;
		case DatePartSpecifier::TIMEZONE_HOUR:
			return ExtractTimezoneHour;
		case DatePartSpecifier::TIMEZONE_MINUTE:
			return ExtractTimezoneMinute;
		default:
			throw InternalException("Unsupported ICU BIGINT extractor");
		}
	}

	static part_double_t PartCodeDoubleFactory(DatePartSpecifier part) {
		switch (part) {
		case DatePartSpecifier::EPOCH:
			return ExtractEpoch;
		case DatePartSpecifier::JULIAN_DAY:
			return ExtractJulianDay;
		default:
			throw InternalException("Unsupported ICU DOUBLE extractor");
		}
	}

	static date_t MakeLastDay(icu::Calendar *calendar, const uint64_t micros) {
		// Set the calendar to midnight on the last day of the month
		calendar->set(UCAL_MILLISECOND, 0);
		calendar->set(UCAL_SECOND, 0);
		calendar->set(UCAL_MINUTE, 0);
		calendar->set(UCAL_HOUR_OF_DAY, 0);

		UErrorCode status = U_ZERO_ERROR;
		const auto dd = calendar->getActualMaximum(UCAL_DATE, status);
		if (U_FAILURE(status)) {
			throw InternalException("Unable to extract ICU last day.");
		}

		calendar->set(UCAL_DATE, dd);

		//	Offset to UTC
		auto millis = calendar->getTime(status);
		millis += ExtractField(calendar, UCAL_ZONE_OFFSET);
		millis += ExtractField(calendar, UCAL_DST_OFFSET);

		return Date::EpochToDate(millis / Interval::MSECS_PER_SEC);
	}

	static string_t MonthName(icu::Calendar *calendar, const uint64_t micros) {
		const auto mm = ExtractMonth(calendar, micros) - 1;
		if (mm == 12) {
			return "Undecimber";
		}
		return Date::MONTH_NAMES[mm];
	}

	static string_t DayName(icu::Calendar *calendar, const uint64_t micros) {
		return Date::DAY_NAMES[ExtractDayOfWeek(calendar, micros)];
	}

	template <typename RESULT_TYPE>
	struct BindAdapterData : public BindData {
		using result_t = RESULT_TYPE;
		typedef result_t (*adapter_t)(icu::Calendar *calendar, const uint64_t micros);
		typedef result_t (*local_adapter_t)(const LocalTime &local);
		using adapters_t = vector<adapter_t>;

		BindAdapterData(ClientContext &context, adapter_t adapter_p, local_adapter_t local_adapter_p)
		    : BindData(context), adapters(1, adapter_p), local_adapter(local_adapter_p) {
		}
		BindAdapterData(ClientContext &context, adapters_t &adapters_p) : BindData(context), adapters(adapters_p) {
		}
		BindAdapterData(const BindAdapterData &other)
		    : BindData(other), adapters(other.adapters), local_adapter(other.local_adapter) {
		}

		adapters_t adapters;
		local_adapter_t local_adapter = nullptr;

		bool Equals(const FunctionData &other_p) const override {
			const auto &other = other_p.Cast<BindAdapterData>();
			return BindData::Equals(other_p) && adapters == other.adapters;
		}

		duckdb::unique_ptr<FunctionData> Copy() const override {
			return make_uniq<BindAdapterData>(*this);
		}
	};

	template <typename INPUT_TYPE, typename RESULT_TYPE>
	static void UnaryTimestampFunction(DataChunk &args, ExpressionState &state, Vector &result) {
		using BIND_TYPE = BindAdapterData<RESULT_TYPE>;
		D_ASSERT(args.ColumnCount() == 1);
		const auto &date_arg = args.data[0];

		auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
		auto &info = func_expr.BindInfo()->Cast<BIND_TYPE>();
		CalendarPtr calendar_ptr;
		auto get_calendar = [&]() {
			if (!calendar_ptr) {
				calendar_ptr.reset(info.calendar->clone());
			}
			return calendar_ptr.get();
		};

		const auto lut = info.local_adapter ? info.lut.get() : nullptr;
		UnaryExecutor::Execute<INPUT_TYPE, RESULT_TYPE>(date_arg, result,
		                                                [&](INPUT_TYPE input) -> optional<RESULT_TYPE> {
			                                                if (!input.IsFinite()) {
				                                                return nullopt;
			                                                }
			                                                LocalTime local;
			                                                if (lut && TryLocalTime(*lut, input, local)) {
				                                                return info.local_adapter(local);
			                                                }
			                                                auto calendar = get_calendar();
			                                                const auto micros = SetTime(calendar, input);
			                                                return info.adapters[0](calendar, micros);
		                                                });
	}

	template <typename INPUT_TYPE, typename RESULT_TYPE>
	static void BinaryTimestampFunction(DataChunk &args, ExpressionState &state, Vector &result) {
		using BIND_TYPE = BindAdapterData<int64_t>;
		D_ASSERT(args.ColumnCount() == 2);
		const auto &part_arg = args.data[0];
		const auto &date_arg = args.data[1];

		auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
		auto &info = func_expr.BindInfo()->Cast<BIND_TYPE>();
		CalendarPtr calendar_ptr;
		auto get_calendar = [&]() {
			if (!calendar_ptr) {
				calendar_ptr.reset(info.calendar->clone());
			}
			return calendar_ptr.get();
		};

		const auto lut = info.lut.get();
		BinaryExecutor::Execute<string_t, INPUT_TYPE, RESULT_TYPE>(
		    part_arg, date_arg, result, [&](string_t specifier, INPUT_TYPE input) -> optional<RESULT_TYPE> {
			    if (!input.IsFinite()) {
				    return nullopt;
			    }
			    const auto part = GetDatePartSpecifier(specifier.GetString());
			    auto adapter = PartCodeBigintFactory(part);
			    LocalTime local;
			    if (lut && TryLocalTime(*lut, input, local)) {
				    if (auto local_adapter = LocalBigintFactory(part)) {
					    return local_adapter(local);
				    }
			    }
			    auto calendar = get_calendar();
			    const auto micros = SetTime(calendar, input);
			    return adapter(calendar, micros);
		    });
	}

	struct BindStructData : public BindData {
		using part_codes_t = vector<DatePartSpecifier>;
		using bigints_t = vector<part_bigint_t>;
		using doubles_t = vector<part_double_t>;

		BindStructData(ClientContext &context, part_codes_t &&part_codes_p)
		    : BindData(context), part_codes(part_codes_p) {
			InitFactories();
		}
		BindStructData(const string &tz_setting_p, const string &cal_setting_p, part_codes_t &&part_codes_p)
		    : BindData(tz_setting_p, cal_setting_p), part_codes(part_codes_p) {
			InitFactories();
		}
		BindStructData(const BindStructData &other)
		    : BindData(other), part_codes(other.part_codes), bigints(other.bigints), doubles(other.doubles),
		      local_bigints(other.local_bigints), local_doubles(other.local_doubles) {
		}

		part_codes_t part_codes;
		bigints_t bigints;
		doubles_t doubles;
		vector<local_bigint_t> local_bigints;
		vector<local_double_t> local_doubles;

		bool Equals(const FunctionData &other_p) const override {
			const auto &other = other_p.Cast<BindStructData>();
			return BindData::Equals(other_p) && part_codes == other.part_codes;
		}

		duckdb::unique_ptr<FunctionData> Copy() const override {
			return make_uniq<BindStructData>(*this);
		}

		void InitFactories() {
			bigints.clear();
			bigints.resize(part_codes.size(), nullptr);
			doubles.clear();
			doubles.resize(part_codes.size(), nullptr);
			local_bigints.clear();
			local_bigints.resize(part_codes.size(), nullptr);
			local_doubles.clear();
			local_doubles.resize(part_codes.size(), nullptr);
			for (size_t col = 0; col < part_codes.size(); ++col) {
				const auto part_code = part_codes[col];
				if (IsBigintDatepart(part_code)) {
					bigints[col] = PartCodeBigintFactory(part_code);
					local_bigints[col] = LocalBigintFactory(part_code);
				} else {
					doubles[col] = PartCodeDoubleFactory(part_code);
					local_doubles[col] = LocalDoubleFactory(part_code);
				}
			}
		}

		bool HasLocalAdapters() const {
			for (size_t col = 0; col < part_codes.size(); ++col) {
				if (!(IsBigintDatepart(part_codes[col]) ? bool(local_bigints[col]) : bool(local_doubles[col]))) {
					return false;
				}
			}
			return true;
		}
	};

	template <typename INPUT_TYPE>
	static void StructFunction(DataChunk &args, ExpressionState &state, Vector &result) {
		auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
		auto &info = func_expr.BindInfo()->Cast<BindStructData>();
		CalendarPtr calendar_ptr;
		auto get_calendar = [&]() {
			if (!calendar_ptr) {
				calendar_ptr.reset(info.calendar->clone());
			}
			return calendar_ptr.get();
		};
		const auto lut = info.HasLocalAdapters() ? info.lut.get() : nullptr;

		D_ASSERT(args.ColumnCount() == 1);
		const auto count = args.size();
		const Vector &input = args.data[0];
		auto entries = input.Values<INPUT_TYPE>();

		result.SetVectorType(VectorType::FLAT_VECTOR);
		auto &child_entries = StructVector::GetEntries(result);
		for (auto &child_entry : child_entries) {
			child_entry.SetVectorType(VectorType::FLAT_VECTOR);
		}

		auto &res_valid = FlatVector::ValidityMutable(result);
		for (idx_t i = 0; i < count; ++i) {
			auto entry = entries[i];
			if (entry.IsValid()) {
				res_valid.SetValid(i);
				const auto is_finite = entry.GetValue().IsFinite();
				LocalTime local;
				const bool use_local = is_finite && lut && TryLocalTime(*lut, entry.GetValue(), local);
				icu::Calendar *calendar = nullptr;
				uint64_t micros = 0;
				if (is_finite && !use_local) {
					calendar = get_calendar();
					micros = SetTime(calendar, entry.GetValue());
				}
				for (size_t col = 0; col < child_entries.size(); ++col) {
					auto &child_entry = child_entries[col];
					if (is_finite) {
						FlatVector::ValidityMutable(child_entry).SetValid(i);
						if (IsBigintDatepart(info.part_codes[col])) {
							auto pdata = FlatVector::GetDataMutable<int64_t>(child_entry);
							pdata[i] = use_local ? info.local_bigints[col](local) : info.bigints[col](calendar, micros);
						} else {
							auto pdata = FlatVector::GetDataMutable<double>(child_entry);
							pdata[i] = use_local ? info.local_doubles[col](local) : info.doubles[col](calendar, micros);
						}
					} else {
						FlatVector::ValidityMutable(child_entry).SetInvalid(i);
					}
				}
			} else {
				res_valid.SetInvalid(i);
				for (auto &child_entry : child_entries) {
					FlatVector::ValidityMutable(child_entry).SetInvalid(i);
				}
			}
		}

		result.Verify();
	}

	template <typename BIND_TYPE>
	static duckdb::unique_ptr<FunctionData> BindAdapter(ClientContext &context, BoundScalarFunction &bound_function,
	                                                    vector<duckdb::unique_ptr<Expression>> &arguments,
	                                                    typename BIND_TYPE::adapter_t adapter,
	                                                    typename BIND_TYPE::local_adapter_t local_adapter = nullptr) {
		return make_uniq<BIND_TYPE>(context, adapter, local_adapter);
	}

	static duckdb::unique_ptr<FunctionData> BindUnaryDatePart(BindScalarFunctionInput &input) {
		auto &bound_function = input.GetBoundFunction();
		auto &context = input.GetClientContext();
		auto &arguments = input.GetArguments();

		const auto part_code = GetDatePartSpecifier(bound_function.GetName().GetIdentifierName());
		if (IsBigintDatepart(part_code)) {
			using data_t = BindAdapterData<int64_t>;
			auto adapter = PartCodeBigintFactory(part_code);
			return BindAdapter<data_t>(context, bound_function, arguments, adapter, LocalBigintFactory(part_code));
		} else {
			using data_t = BindAdapterData<double>;
			auto adapter = PartCodeDoubleFactory(part_code);
			return BindAdapter<data_t>(context, bound_function, arguments, adapter, LocalDoubleFactory(part_code));
		}
	}

	static duckdb::unique_ptr<FunctionData> BindBinaryDatePart(BindScalarFunctionInput &input) {
		auto &bound_function = input.GetBoundFunction();
		auto &context = input.GetClientContext();
		auto &arguments = input.GetArguments();

		//	If we are only looking for Julian Days, then patch in the unary function.
		do {
			if (arguments[0]->HasParameter() || !arguments[0]->IsFoldable()) {
				break;
			}

			Value part_value = ExpressionExecutor::EvaluateScalar(context, *arguments[0]);
			if (part_value.IsNull()) {
				break;
			}

			const auto part_name = part_value.ToString();
			const auto part_code = GetDatePartSpecifier(part_name);
			if (IsBigintDatepart(part_code)) {
				break;
			}

			arguments.erase(arguments.begin());
			bound_function.GetArguments().erase(bound_function.GetArguments().begin());
			bound_function.SetName(Identifier(part_name));
			bound_function.SetReturnType(LogicalType::DOUBLE);
			bound_function.SetFunctionCallback(UnaryTimestampFunction<timestamp_tz_t, double>);

			return BindUnaryDatePart(input);
		} while (false);

		using data_t = BindAdapterData<int64_t>;
		return BindAdapter<data_t>(context, bound_function, arguments, nullptr);
	}

	static duckdb::unique_ptr<FunctionData> BindStruct(BindScalarFunctionInput &input) {
		auto &bound_function = input.GetBoundFunction();
		auto &context = input.GetClientContext();
		auto &arguments = input.GetArguments();

		// collect names and deconflict, construct return type
		if (arguments[0]->HasParameter()) {
			throw ParameterNotResolvedException();
		}
		if (!arguments[0]->IsFoldable()) {
			throw BinderException("%s can only take constant lists of part names", bound_function.GetName());
		}

		case_insensitive_set_t name_collision_set;
		child_list_t<LogicalType> struct_children;
		BindStructData::part_codes_t part_codes;

		Value parts_list = ExpressionExecutor::EvaluateScalar(context, *arguments[0]);
		if (parts_list.type().id() == LogicalTypeId::LIST) {
			auto &list_children = ListValue::GetChildren(parts_list);
			if (list_children.empty()) {
				throw BinderException("%s requires non-empty lists of part names", bound_function.GetName());
			}

			for (size_t col = 0; col < list_children.size(); ++col) {
				const auto &part_value = list_children[col];
				if (part_value.IsNull()) {
					throw BinderException("NULL struct entry name in %s", bound_function.GetName());
				}
				const auto part_name = part_value.ToString();
				const auto part_code = GetDatePartSpecifier(part_name);
				if (name_collision_set.find(part_name) != name_collision_set.end()) {
					throw BinderException("Duplicate struct entry name \"%s\" in %s", part_name,
					                      bound_function.GetName());
				}
				name_collision_set.insert(part_name);
				part_codes.emplace_back(part_code);
				if (IsBigintDatepart(part_code)) {
					struct_children.emplace_back(make_pair(part_name, LogicalType::BIGINT));
				} else {
					struct_children.emplace_back(make_pair(part_name, LogicalType::DOUBLE));
				}
			}
		} else {
			throw BinderException("%s can only take constant lists of part names", bound_function.GetName());
		}

		Function::EraseArgument(bound_function, arguments, 0);
		bound_function.SetReturnType(LogicalType::STRUCT(std::move(struct_children)));
		return make_uniq<BindStructData>(context, std::move(part_codes));
	}

	static void SerializeStructFunction(Serializer &serializer, const optional_ptr<FunctionData> bind_data,
	                                    const BoundScalarFunction &function) {
		D_ASSERT(bind_data);
		auto &info = bind_data->Cast<BindStructData>();
		serializer.WriteProperty(100, "tz_setting", info.tz_setting);
		serializer.WriteProperty(101, "cal_setting", info.cal_setting);
		serializer.WriteProperty(102, "part_codes", info.part_codes);
	}

	static duckdb::unique_ptr<FunctionData> DeserializeStructFunction(Deserializer &deserializer,
	                                                                  BoundScalarFunction &bound_function) {
		auto tz_setting = deserializer.ReadProperty<string>(100, "tz_setting");
		auto cal_setting = deserializer.ReadProperty<string>(101, "cal_setting");
		auto part_codes = deserializer.ReadProperty<vector<DatePartSpecifier>>(102, "part_codes");
		return make_uniq<BindStructData>(tz_setting, cal_setting, std::move(part_codes));
	}

	template <typename INPUT_TYPE, typename RESULT_TYPE>
	static ScalarFunction GetUnaryPartCodeFunction(const LogicalType &temporal_type,
	                                               const LogicalType &result_type = LogicalType::BIGINT) {
		return ScalarFunction({temporal_type}, result_type, UnaryTimestampFunction<INPUT_TYPE, RESULT_TYPE>,
		                      BindUnaryDatePart);
	}

	template <typename RESULT_TYPE = int64_t>
	static void AddUnaryPartCodeFunctions(const Identifier &name, ExtensionLoader &loader,
	                                      const LogicalType &result_type = LogicalType::BIGINT,
	                                      ArgProperties unary_arg0_props = {}) {
		ScalarFunctionSet set {name};
		set.AddFunction(GetUnaryPartCodeFunction<timestamp_tz_t, RESULT_TYPE>(LogicalType::TIMESTAMP_TZ, result_type));
		set.SetUnaryArgProperties(unary_arg0_props);
		loader.RegisterFunction(set);
	}

	template <typename INPUT_TYPE, typename RESULT_TYPE>
	static ScalarFunction GetBinaryPartCodeFunction(const LogicalType &temporal_type) {
		return ScalarFunction({LogicalType::VARCHAR, temporal_type}, LogicalType::BIGINT,
		                      BinaryTimestampFunction<INPUT_TYPE, RESULT_TYPE>, BindBinaryDatePart);
	}

	template <typename INPUT_TYPE>
	static ScalarFunction GetStructFunction(const LogicalType &temporal_type) {
		auto part_type = LogicalType::LIST(LogicalType::VARCHAR);
		auto result_type = LogicalType::STRUCT({});
		ScalarFunction result({part_type, temporal_type}, result_type, StructFunction<INPUT_TYPE>, BindStruct);
		result.SetSerializeCallback(SerializeStructFunction);
		result.SetDeserializeCallback(DeserializeStructFunction);
		return result;
	}

	static void AddDatePartFunctions(const Identifier &name, ExtensionLoader &loader) {
		ScalarFunctionSet set {name};
		set.AddFunction(GetBinaryPartCodeFunction<timestamp_tz_t, int64_t>(LogicalType::TIMESTAMP_TZ));
		set.AddFunction(GetStructFunction<timestamp_tz_t>(LogicalType::TIMESTAMP_TZ));
		for (auto &func : set.functions) {
			func.SetFallible();
		}
		loader.RegisterFunction(set);
	}

	static duckdb::unique_ptr<FunctionData> BindLastDate(BindScalarFunctionInput &input) {
		auto &bound_function = input.GetBoundFunction();
		auto &context = input.GetClientContext();
		auto &arguments = input.GetArguments();

		using data_t = BindAdapterData<date_t>;
		return BindAdapter<data_t>(context, bound_function, arguments, MakeLastDay, LocalLastDay);
	}

	template <typename INPUT_TYPE>
	static ScalarFunction GetLastDayFunction(const LogicalType &temporal_type) {
		return ScalarFunction({temporal_type}, LogicalType::DATE, UnaryTimestampFunction<INPUT_TYPE, date_t>,
		                      BindLastDate);
	}
	static void AddLastDayFunctions(const Identifier &name, ExtensionLoader &loader) {
		ScalarFunctionSet set {name};
		set.AddFunction(GetLastDayFunction<timestamp_tz_t>(LogicalType::TIMESTAMP_TZ));
		loader.RegisterFunction(set);
	}

	static unique_ptr<FunctionData> BindMonthName(BindScalarFunctionInput &input) {
		auto &context = input.GetClientContext();
		auto &bound_function = input.GetBoundFunction();
		auto &arguments = input.GetArguments();
		using data_t = BindAdapterData<string_t>;
		return BindAdapter<data_t>(context, bound_function, arguments, MonthName, LocalMonthName);
	}

	template <typename INPUT_TYPE>
	static ScalarFunction GetMonthNameFunction(const LogicalType &temporal_type) {
		return ScalarFunction({temporal_type}, LogicalType::VARCHAR, UnaryTimestampFunction<INPUT_TYPE, string_t>,
		                      BindMonthName);
	}
	static void AddMonthNameFunctions(const Identifier &name, ExtensionLoader &loader) {
		ScalarFunctionSet set {name};
		set.AddFunction(GetMonthNameFunction<timestamp_tz_t>(LogicalType::TIMESTAMP_TZ));
		loader.RegisterFunction(set);
	}

	static unique_ptr<FunctionData> BindDayName(BindScalarFunctionInput &input) {
		auto &context = input.GetClientContext();
		auto &bound_function = input.GetBoundFunction();
		auto &arguments = input.GetArguments();
		using data_t = BindAdapterData<string_t>;
		return BindAdapter<data_t>(context, bound_function, arguments, DayName, LocalDayName);
	}

	template <typename INPUT_TYPE>
	static ScalarFunction GetDayNameFunction(const LogicalType &temporal_type) {
		return ScalarFunction({temporal_type}, LogicalType::VARCHAR, UnaryTimestampFunction<INPUT_TYPE, string_t>,
		                      BindDayName);
	}
	static void AddDayNameFunctions(const Identifier &name, ExtensionLoader &loader) {
		ScalarFunctionSet set {name};
		set.AddFunction(GetDayNameFunction<timestamp_tz_t>(LogicalType::TIMESTAMP_TZ));
		loader.RegisterFunction(set);
	}
};

void RegisterICUDatePartFunctions(ExtensionLoader &loader) {
	// register the individual operators

	// year/decade use UCAL_YEAR (year-of-era, positive in both BC and AD), which is non-monotonic
	// across the BC/AD flip; leave them unannotated. era/century/millennium/isoyear are signed.

	//	BIGINTs
	ICUDatePart::AddUnaryPartCodeFunctions("era", loader, LogicalType::BIGINT, ArgProperties().NonDecreasing());
	ICUDatePart::AddUnaryPartCodeFunctions("year", loader);
	ICUDatePart::AddUnaryPartCodeFunctions("month", loader);
	ICUDatePart::AddUnaryPartCodeFunctions("day", loader);
	ICUDatePart::AddUnaryPartCodeFunctions("decade", loader);
	ICUDatePart::AddUnaryPartCodeFunctions("century", loader, LogicalType::BIGINT, ArgProperties().NonDecreasing());
	ICUDatePart::AddUnaryPartCodeFunctions("millennium", loader, LogicalType::BIGINT, ArgProperties().NonDecreasing());
	ICUDatePart::AddUnaryPartCodeFunctions("microsecond", loader);
	ICUDatePart::AddUnaryPartCodeFunctions("millisecond", loader);
	ICUDatePart::AddUnaryPartCodeFunctions("second", loader);
	ICUDatePart::AddUnaryPartCodeFunctions("minute", loader);
	ICUDatePart::AddUnaryPartCodeFunctions("hour", loader);
	ICUDatePart::AddUnaryPartCodeFunctions("dayofweek", loader);
	ICUDatePart::AddUnaryPartCodeFunctions("isodow", loader);
	ICUDatePart::AddUnaryPartCodeFunctions("week", loader); //  Note that WeekOperator is ISO-8601, not US
	ICUDatePart::AddUnaryPartCodeFunctions("dayofyear", loader);
	ICUDatePart::AddUnaryPartCodeFunctions("quarter", loader);
	ICUDatePart::AddUnaryPartCodeFunctions("isoyear", loader, LogicalType::BIGINT, ArgProperties().NonDecreasing());
	ICUDatePart::AddUnaryPartCodeFunctions("timezone", loader);
	ICUDatePart::AddUnaryPartCodeFunctions("timezone_hour", loader);
	ICUDatePart::AddUnaryPartCodeFunctions("timezone_minute", loader);

	//	DOUBLEs
	ICUDatePart::AddUnaryPartCodeFunctions<double>("epoch", loader, LogicalType::DOUBLE);
	ICUDatePart::AddUnaryPartCodeFunctions<double>("julian", loader, LogicalType::DOUBLE);

	//  register combinations
	ICUDatePart::AddUnaryPartCodeFunctions("yearweek", loader, LogicalType::BIGINT,
	                                       ArgProperties().NonDecreasing()); //  ISO year and week

	//  register various aliases
	ICUDatePart::AddUnaryPartCodeFunctions("dayofmonth", loader);
	ICUDatePart::AddUnaryPartCodeFunctions("weekday", loader);
	ICUDatePart::AddUnaryPartCodeFunctions("weekofyear", loader);

	//  register the last_day function
	ICUDatePart::AddLastDayFunctions("last_day", loader);

	// register the dayname/monthname functions
	ICUDatePart::AddMonthNameFunctions("monthname", loader);
	ICUDatePart::AddDayNameFunctions("dayname", loader);

	// finally the actual date_part function
	ICUDatePart::AddDatePartFunctions("date_part", loader);
	ICUDatePart::AddDatePartFunctions("datepart", loader);
}

} // namespace duckdb
