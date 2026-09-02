#include "include/icu-datetrunc.hpp"
#include "include/icu-datefunc.hpp"
#include "include/icu-zone-lut.hpp"

#include "duckdb/common/helper.hpp"
#include "duckdb/common/operator/date_trunc_operators.hpp"
#include "duckdb/common/vector_operations/binary_executor.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/transaction/meta_transaction.hpp"
#include "unicode/basictz.h"

namespace duckdb {

template <class OP>
struct DateTruncRecomputes {
	using type = void;
};

template <>
struct DateTruncRecomputes<DateTrunc::QuarterOperator> {
	using type = DateTrunc::MonthOperator;
};

template <>
struct DateTruncRecomputes<DateTrunc::DecadeOperator> {
	using type = DateTrunc::YearOperator;
};

template <>
struct DateTruncRecomputes<DateTrunc::CenturyOperator> {
	using type = DateTrunc::YearOperator;
};

template <>
struct DateTruncRecomputes<DateTrunc::MillenniumOperator> {
	using type = DateTrunc::YearOperator;
};

struct ICUDateTrunc : public ICUDateFunc {
	struct FixedOffset {
		int64_t micros;
		int64_t min_input;
		int64_t max_input;
		int64_t min_truncated;
	};

	static FixedOffset MakeFixedOffset(int64_t micros) {
		const auto limit = NumericLimits<int64_t>::Maximum() - 1;
		FixedOffset fixed;
		fixed.micros = micros;
		fixed.min_input = -limit - MinValue<int64_t>(micros, 0);
		fixed.max_input = limit - MaxValue<int64_t>(micros, 0);
		fixed.min_truncated = -limit + MaxValue<int64_t>(micros, 0);
		return fixed;
	}

	template <class OP>
	static constexpr bool UsesEraYear() {
		return std::is_same<OP, DateTrunc::DecadeOperator>::value ||
		       std::is_same<OP, DateTrunc::CenturyOperator>::value ||
		       std::is_same<OP, DateTrunc::MillenniumOperator>::value;
	}

	static inline const int64_t FIRST_ANNO_DOMINI = DateTrunc::FromDays(DateTrunc::YearStart(1)).value;

	template <class OP>
	[[gnu::always_inline]] static inline bool TryTruncFixedOffset(const FixedOffset &fixed, timestamp_tz_t input,
	                                                              timestamp_tz_t &result) {
		if (DUCKDB_UNLIKELY(input.value < fixed.min_input || input.value > fixed.max_input)) {
			throw ConversionException("ICU date overflows timestamp range");
		}
		const timestamp_t wall(input.value + fixed.micros);
		if constexpr (UsesEraYear<OP>()) {
			if (wall.value < FIRST_ANNO_DOMINI) {
				return false;
			}
		}
		const auto truncated = OP::template Operation<timestamp_t, timestamp_t>(wall);
		if (DUCKDB_UNLIKELY(truncated.value < fixed.min_truncated)) {
			throw ConversionException("ICU date overflows timestamp range");
		}
		result = timestamp_tz_t(truncated.value - fixed.micros);
		return true;
	}

	template <class OP>
	static constexpr bool PreservesOffset() {
		return std::is_same<OP, DateTrunc::MinuteOperator>::value ||
		       std::is_same<OP, DateTrunc::SecondOperator>::value ||
		       std::is_same<OP, DateTrunc::MillisecondOperator>::value ||
		       std::is_same<OP, DateTrunc::MicrosecondOperator>::value;
	}

	[[gnu::always_inline]] static inline bool InGap(const ZoneLUT &lut, int64_t wall) {
		const auto wall_day = lut.WallDay(wall);
		if (!wall_day) {
			return true;
		}
		const auto instant = ZoneLUT::Resolve(*wall_day, wall);
		const auto instant_day = lut.InstantDay(instant);
		return !instant_day || instant + ZoneLUT::Offset(*instant_day, instant) != wall;
	}

	template <class OP>
	[[gnu::always_inline]] static inline bool TryTruncate(const ZoneLUT &lut, timestamp_tz_t input,
	                                                      timestamp_tz_t &result) {
		const auto instant_day = lut.InstantDay(input.value);
		if (!instant_day) {
			return false;
		}
		const auto offset = ZoneLUT::Offset(*instant_day, input.value);
		const timestamp_t wall(input.value + offset);
		const auto truncated = OP::template Operation<timestamp_t, timestamp_t>(wall);
		if constexpr (PreservesOffset<OP>()) {
			result = timestamp_tz_t(truncated.value - offset);
			return true;
		}
		using INTERMEDIATE = typename DateTruncRecomputes<OP>::type;
		if constexpr (!std::is_void<INTERMEDIATE>::value) {
			if (InGap(lut, INTERMEDIATE::template Operation<timestamp_t, timestamp_t>(wall).value)) {
				return false;
			}
		}
		const auto wall_day = lut.WallDay(truncated.value);
		if (!wall_day) {
			return false;
		}
		result = timestamp_tz_t(ZoneLUT::Resolve(*wall_day, truncated.value));
		return true;
	}

	static void PreserveOffsets(icu::Calendar *calendar) {
		//	We have to extract _everything_ before setting anything
		//	Otherwise ICU will clear the fStamp fields
		//	This also means we must call this method first.

		//	Force reuse of offsets when reassembling truncated sub-hour times.
		const auto zone_offset = ExtractField(calendar, UCAL_ZONE_OFFSET);
		const auto dst_offset = ExtractField(calendar, UCAL_DST_OFFSET);

		calendar->set(UCAL_ZONE_OFFSET, zone_offset);
		calendar->set(UCAL_DST_OFFSET, dst_offset);
	}

	static void TruncMicrosecondInternal(icu::Calendar *calendar, uint64_t &micros) {
	}

	static void TruncMicrosecond(icu::Calendar *calendar, uint64_t &micros) {
		PreserveOffsets(calendar);
		TruncMicrosecondInternal(calendar, micros);
	}

	static void TruncMillisecondInternal(icu::Calendar *calendar, uint64_t &micros) {
		TruncMicrosecondInternal(calendar, micros);
		micros = 0;
	}

	static void TruncMillisecond(icu::Calendar *calendar, uint64_t &micros) {
		PreserveOffsets(calendar);
		TruncMillisecondInternal(calendar, micros);
	}

	static void TruncSecondInternal(icu::Calendar *calendar, uint64_t &micros) {
		TruncMillisecondInternal(calendar, micros);
		calendar->set(UCAL_MILLISECOND, 0);
	}

	static void TruncSecond(icu::Calendar *calendar, uint64_t &micros) {
		PreserveOffsets(calendar);
		TruncSecondInternal(calendar, micros);
	}

	static void TruncMinuteInternal(icu::Calendar *calendar, uint64_t &micros) {
		TruncSecondInternal(calendar, micros);
		calendar->set(UCAL_SECOND, 0);
	}

	static void TruncMinute(icu::Calendar *calendar, uint64_t &micros) {
		PreserveOffsets(calendar);
		TruncMinuteInternal(calendar, micros);
	}

	static void TruncHour(icu::Calendar *calendar, uint64_t &micros) {
		TruncMinuteInternal(calendar, micros);
		calendar->set(UCAL_MINUTE, 0);
	}

	static void TruncDay(icu::Calendar *calendar, uint64_t &micros) {
		TruncHour(calendar, micros);
		calendar->set(UCAL_HOUR_OF_DAY, 0);
	}

	static void TruncWeek(icu::Calendar *calendar, uint64_t &micros) {
		calendar->setFirstDayOfWeek(UCAL_MONDAY);
		calendar->setMinimalDaysInFirstWeek(4);
		TruncDay(calendar, micros);
		calendar->set(UCAL_DAY_OF_WEEK, UCAL_MONDAY);
	}

	static void TruncMonth(icu::Calendar *calendar, uint64_t &micros) {
		TruncDay(calendar, micros);
		calendar->set(UCAL_DATE, 1);
	}

	static void TruncQuarter(icu::Calendar *calendar, uint64_t &micros) {
		TruncMonth(calendar, micros);
		auto mm = ExtractField(calendar, UCAL_MONTH);
		calendar->set(UCAL_MONTH, (mm / 3) * 3);
	}

	static void TruncYear(icu::Calendar *calendar, uint64_t &micros) {
		TruncMonth(calendar, micros);
		calendar->set(UCAL_MONTH, UCAL_JANUARY);
	}

	static void TruncISOYear(icu::Calendar *calendar, uint64_t &micros) {
		TruncWeek(calendar, micros);
		calendar->set(UCAL_WEEK_OF_YEAR, 1);
	}

	static void TruncDecade(icu::Calendar *calendar, uint64_t &micros) {
		TruncYear(calendar, micros);
		auto yyyy = ExtractField(calendar, UCAL_YEAR) / 10;
		calendar->set(UCAL_YEAR, yyyy * 10);
	}

	static void TruncCentury(icu::Calendar *calendar, uint64_t &micros) {
		TruncYear(calendar, micros);
		auto yyyy = ExtractField(calendar, UCAL_YEAR) / 100;
		calendar->set(UCAL_YEAR, yyyy * 100);
	}

	static void TruncMillenium(icu::Calendar *calendar, uint64_t &micros) {
		TruncYear(calendar, micros);
		auto yyyy = ExtractField(calendar, UCAL_YEAR) / 1000;
		calendar->set(UCAL_YEAR, yyyy * 1000);
	}

	static void TruncEra(icu::Calendar *calendar, uint64_t &micros) {
		TruncYear(calendar, micros);
		auto era = ExtractField(calendar, UCAL_ERA);
		calendar->set(UCAL_YEAR, 0);
		calendar->set(UCAL_ERA, era);
	}

	template <typename T>
	static void ICUDateTruncFunction(DataChunk &args, ExpressionState &state, Vector &result) {
		D_ASSERT(args.ColumnCount() == 2);
		const auto &part_arg = args.data[0];
		const auto &date_arg = args.data[1];

		auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
		auto &info = func_expr.BindInfo()->Cast<BindData>();
		CalendarPtr calendar;
		auto get_calendar = [&]() {
			if (!calendar) {
				calendar.reset(info.calendar->clone());
			}
			return calendar.get();
		};

		const bool gregorian = strcmp(info.calendar->getType(), "gregorian") == 0;
		const auto lut = gregorian ? ZoneLUT::Get(info.calendar->getTimeZone()) : nullptr;
		const bool has_fixed_offset = lut && lut->HasFixedOffset();
		const FixedOffset fixed = has_fixed_offset ? MakeFixedOffset(lut->FixedOffset()) : FixedOffset();
		auto use_fixed_offset = [&](DatePartSpecifier type) {
			return has_fixed_offset && type != DatePartSpecifier::ERA;
		};
		auto use_lut = [&](DatePartSpecifier type) {
			return lut && !has_fixed_offset && type != DatePartSpecifier::ERA;
		};
		auto icu_truncate = [&](part_trunc_t truncator, T input) {
			auto icu_calendar = get_calendar();
			auto micros = SetTime(icu_calendar, input);
			truncator(icu_calendar, micros);
			return GetTimeUnsafe(icu_calendar, micros);
		};

		if (part_arg.GetVectorType() == VectorType::CONSTANT_VECTOR) {
			// Common case of constant part.
			if (ConstantVector::IsNull(part_arg)) {
				throw InternalException("ICUDateTrunc called with constant NULL bucket width");
			}
			const auto specifier = ConstantVector::GetData<string_t>(part_arg)->GetString();
			const auto type = GetDatePartSpecifier(specifier);
			auto truncator = TruncationFactory(type);
			if (use_fixed_offset(type)) {
				DateTrunc::Dispatch(type, [&](auto op) {
					UnaryExecutor::Execute<T, T>(date_arg, result, [&](T input) {
						T truncated;
						if (!input.IsFinite()) {
							return input;
						}
						if (TryTruncFixedOffset<decltype(op)>(fixed, input, truncated)) {
							return truncated;
						}
						return icu_truncate(truncator, input);
					});
				});
				return;
			}
			if (use_lut(type)) {
				DateTrunc::Dispatch(type, [&](auto op) {
					UnaryExecutor::Execute<T, T>(date_arg, result, [&](T input) {
						T truncated;
						if (!input.IsFinite()) {
							return input;
						}
						if (TryTruncate<decltype(op)>(*lut, input, truncated)) {
							return truncated;
						}
						return icu_truncate(truncator, input);
					});
				});
				return;
			}
			UnaryExecutor::Execute<T, T>(
			    date_arg, result, [&](T input) { return input.IsFinite() ? icu_truncate(truncator, input) : input; });
		} else {
			BinaryExecutor::Execute<string_t, T, T>(part_arg, date_arg, result, [&](string_t specifier, T input) {
				const auto type = GetDatePartSpecifier(specifier.GetString());
				auto truncator = TruncationFactory(type);
				if (!input.IsFinite()) {
					return input;
				}
				T truncated;
				if (use_fixed_offset(type) && DateTrunc::Dispatch(type, [&](auto op) {
					    return TryTruncFixedOffset<decltype(op)>(fixed, input, truncated);
				    })) {
					return truncated;
				}
				if (use_lut(type) && DateTrunc::Dispatch(type, [&](auto op) {
					    return TryTruncate<decltype(op)>(*lut, input, truncated);
				    })) {
					return truncated;
				}
				return icu_truncate(truncator, input);
			});
		}
	}

	template <typename TA>
	static ScalarFunction GetDateTruncFunction(const LogicalTypeId &type) {
		return ScalarFunction({LogicalType::VARCHAR, type}, LogicalType::TIMESTAMP_TZ, ICUDateTruncFunction<TA>, Bind);
	}

	static void AddBinaryTimestampFunction(const Identifier &name, ExtensionLoader &loader) {
		ScalarFunctionSet set {name};
		set.AddFunction(GetDateTruncFunction<timestamp_tz_t>(LogicalType::TIMESTAMP_TZ));
		set.SetArgProperties(1, ArgProperties().NonDecreasing());
		loader.RegisterFunction(set);
	}
};

ICUDateFunc::part_trunc_t ICUDateFunc::TruncationFactory(DatePartSpecifier type) {
	switch (type) {
	case DatePartSpecifier::ERA:
		return ICUDateTrunc::TruncEra;
	case DatePartSpecifier::MILLENNIUM:
		return ICUDateTrunc::TruncMillenium;
	case DatePartSpecifier::CENTURY:
		return ICUDateTrunc::TruncCentury;
	case DatePartSpecifier::DECADE:
		return ICUDateTrunc::TruncDecade;
	case DatePartSpecifier::YEAR:
		return ICUDateTrunc::TruncYear;
	case DatePartSpecifier::QUARTER:
		return ICUDateTrunc::TruncQuarter;
	case DatePartSpecifier::MONTH:
		return ICUDateTrunc::TruncMonth;
	case DatePartSpecifier::WEEK:
	case DatePartSpecifier::YEARWEEK:
		return ICUDateTrunc::TruncWeek;
	case DatePartSpecifier::ISOYEAR:
		return ICUDateTrunc::TruncISOYear;
	case DatePartSpecifier::DAY:
	case DatePartSpecifier::DOW:
	case DatePartSpecifier::ISODOW:
	case DatePartSpecifier::DOY:
	case DatePartSpecifier::JULIAN_DAY:
		return ICUDateTrunc::TruncDay;
	case DatePartSpecifier::HOUR:
		return ICUDateTrunc::TruncHour;
	case DatePartSpecifier::MINUTE:
		return ICUDateTrunc::TruncMinute;
	case DatePartSpecifier::SECOND:
	case DatePartSpecifier::EPOCH:
		return ICUDateTrunc::TruncSecond;
	case DatePartSpecifier::MILLISECONDS:
		return ICUDateTrunc::TruncMillisecond;
	case DatePartSpecifier::MICROSECONDS:
		return ICUDateTrunc::TruncMicrosecond;
	default:
		throw NotImplementedException("Specifier type not implemented for ICU DATETRUNC");
	}
}

timestamp_tz_t ICUDateFunc::CurrentMidnight(icu::Calendar *calendar, ExpressionState &state) {
	const timestamp_tz_t current_timestamp(MetaTransaction::Get(state.GetContext()).start_timestamp);
	auto current_micros = SetTime(calendar, current_timestamp);
	ICUDateTrunc::TruncDay(calendar, current_micros);
	return GetTime(calendar);
}

void RegisterICUDateTruncFunctions(ExtensionLoader &loader) {
	ICUDateTrunc::AddBinaryTimestampFunction("date_trunc", loader);
	ICUDateTrunc::AddBinaryTimestampFunction("datetrunc", loader);
}

} // namespace duckdb
