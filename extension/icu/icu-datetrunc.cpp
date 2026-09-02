#include "include/icu-datetrunc.hpp"
#include "include/icu-datefunc.hpp"
#include "include/icu-zone-lut.hpp"

#include "duckdb/common/operator/date_trunc_operators.hpp"
#include "duckdb/common/vector_operations/binary_executor.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"
#include "duckdb/transaction/meta_transaction.hpp"

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
	template <class OP>
	static constexpr bool UsesEraYear() {
		return std::is_same<OP, DateTrunc::DecadeOperator>::value ||
		       std::is_same<OP, DateTrunc::CenturyOperator>::value ||
		       std::is_same<OP, DateTrunc::MillenniumOperator>::value;
	}

	template <class OP>
	static constexpr bool PreservesOffset() {
		return std::is_same<OP, DateTrunc::MinuteOperator>::value ||
		       std::is_same<OP, DateTrunc::SecondOperator>::value ||
		       std::is_same<OP, DateTrunc::MillisecondOperator>::value ||
		       std::is_same<OP, DateTrunc::MicrosecondOperator>::value;
	}

	template <class OP, class = void>
	struct TruncatesDays : std::false_type {};

	template <class OP>
	struct TruncatesDays<OP, std::void_t<decltype(OP::Days(int32_t()))>> : std::true_type {};

	[[gnu::always_inline]] static inline bool InGap(const ZoneLUT &lut, int64_t days) {
		const int64_t wall = days * Interval::MICROS_PER_DAY;
		int64_t instant = 0;
		int64_t instant_day = 0;
		int64_t offset = 0;
		if (!lut.TryResolveDay(days - ZoneLUT::FIRST_DAY, wall, instant) ||
		    !lut.TryInstantDay(instant, instant_day, offset)) {
			return true;
		}
		return instant + offset != wall;
	}

	template <class OP>
	[[gnu::always_inline]] static inline bool TryTruncate(const ZoneLUT &lut, timestamp_tz_t input,
	                                                      timestamp_tz_t &result) {
		int64_t offset = 0;
		int64_t instant_day = 0;
		if (!lut.TryInstantDay(input.value, instant_day, offset)) {
			return false;
		}
		const timestamp_t wall(input.value + offset);
		if constexpr (UsesEraYear<OP>()) {
			if (wall.value < ZoneLUT::FIRST_ANNO_DOMINI) {
				return false;
			}
		}
		if constexpr (TruncatesDays<OP>::value) {
			const auto days = DateTrunc::ToDays(wall);
			using INTERMEDIATE = typename DateTruncRecomputes<OP>::type;
			if constexpr (!std::is_void<INTERMEDIATE>::value) {
				if (InGap(lut, INTERMEDIATE::Days(days))) {
					return false;
				}
			}
			const auto truncated_days = OP::Days(days);
			return lut.TryResolveDay(truncated_days - ZoneLUT::FIRST_DAY, truncated_days * Interval::MICROS_PER_DAY,
			                         result.value);
		} else {
			const auto truncated = OP::template Operation<timestamp_t, timestamp_t>(wall);
			if constexpr (PreservesOffset<OP>()) {
				return lut.TryShiftBack(truncated.value, offset, result.value);
			}
			if (lut.HasFixedOffset()) {
				return lut.TryResolve(truncated.value, result.value);
			}
			const auto start = ZoneLUT::DayStart(instant_day);
			const auto wall_day = instant_day + (wall.value >= start + Interval::MICROS_PER_DAY) - (wall.value < start);
			return lut.TryResolveDay(wall_day, truncated.value, result.value);
		}
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

		const auto lut = info.lut.get();
		auto use_lut = [&](DatePartSpecifier type) {
			return lut && type != DatePartSpecifier::ERA;
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
				if (use_lut(type) && DateTrunc::Dispatch(type, [&](auto op) {
					    return TryTruncate<decltype(op)>(*lut, input, truncated);
				    })) {
					return truncated;
				}
				return icu_truncate(truncator, input);
			});
		}
	}

	struct BucketSpec {
		enum class Kind : uint8_t { INSTANT, LOCAL_DAY, LOCAL_MONTH };
		Kind kind = Kind::INSTANT;
		DatePartSpecifier part = DatePartSpecifier::HOUR;
		int64_t width = 1;
		int64_t anchor = 0;
	};

	static bool TryGetBucketSpec(DatePartSpecifier part, BucketSpec &spec) {
		spec = BucketSpec();
		spec.part = part;
		switch (part) {
		case DatePartSpecifier::MICROSECONDS:
			return true;
		case DatePartSpecifier::MILLISECONDS:
			spec.width = Interval::MICROS_PER_MSEC;
			return true;
		case DatePartSpecifier::SECOND:
		case DatePartSpecifier::EPOCH:
			spec.width = Interval::MICROS_PER_SEC;
			return true;
		case DatePartSpecifier::MINUTE:
			spec.width = Interval::MICROS_PER_MINUTE;
			return true;
		case DatePartSpecifier::HOUR:
			spec.width = Interval::MICROS_PER_HOUR;
			return true;
		case DatePartSpecifier::DAY:
		case DatePartSpecifier::DOW:
		case DatePartSpecifier::ISODOW:
		case DatePartSpecifier::DOY:
		case DatePartSpecifier::JULIAN_DAY:
			spec.kind = BucketSpec::Kind::LOCAL_DAY;
			return true;
		case DatePartSpecifier::WEEK:
		case DatePartSpecifier::YEARWEEK:
			spec.kind = BucketSpec::Kind::LOCAL_DAY;
			spec.width = Interval::DAYS_PER_WEEK;
			spec.anchor = DateTrunc::EPOCH_MONDAY;
			return true;
		case DatePartSpecifier::MONTH:
			spec.kind = BucketSpec::Kind::LOCAL_MONTH;
			return true;
		case DatePartSpecifier::QUARTER:
			spec.kind = BucketSpec::Kind::LOCAL_MONTH;
			spec.width = Interval::MONTHS_PER_QUARTER;
			return true;
		case DatePartSpecifier::YEAR:
			spec.kind = BucketSpec::Kind::LOCAL_MONTH;
			spec.width = Interval::MONTHS_PER_YEAR;
			return true;
		case DatePartSpecifier::DECADE:
			spec.kind = BucketSpec::Kind::LOCAL_MONTH;
			spec.width = Interval::MONTHS_PER_DECADE;
			return true;
		case DatePartSpecifier::CENTURY:
			spec.kind = BucketSpec::Kind::LOCAL_MONTH;
			spec.width = Interval::MONTHS_PER_CENTURY;
			return true;
		case DatePartSpecifier::MILLENNIUM:
			spec.kind = BucketSpec::Kind::LOCAL_MONTH;
			spec.width = Interval::MONTHS_PER_MILLENIUM;
			return true;
		default:
			return false;
		}
	}

	[[noreturn]] static void ThrowBucketRange() {
		throw InvalidInputException("Time zone bucket outside the supported range: the statistics of the input may be "
		                            "corrupt (PRAGMA disable_optimizer to disable optimizations that rely on them)");
	}

	[[gnu::always_inline]] static inline int64_t FixUpHour(const ZoneDay &entry, int64_t hour) {
		if (entry.transition == ZoneLUT::NO_TRANSITION) {
			return hour;
		}
		const int64_t duplicated = int64_t(entry.before - entry.after) * Interval::MICROS_PER_SEC;
		if (duplicated > 0 && hour >= entry.transition - duplicated && hour < entry.transition) {
			return hour + duplicated;
		}
		return hour;
	}

	[[gnu::always_inline]] static inline int64_t LocalDay(const ZoneLUT &lut, int64_t micros) {
		int64_t day = 0;
		int64_t offset = 0;
		if (!lut.TryInstantDay(micros, day, offset)) {
			ThrowBucketRange();
		}
		return DateTrunc::FloorDiv(micros + offset, Interval::MICROS_PER_DAY);
	}

	[[gnu::always_inline]] static inline int64_t HourWithTransitions(const ZoneLUT &lut, int64_t micros) {
		const auto hour = DateTrunc::FloorDiv(micros, Interval::MICROS_PER_HOUR) * Interval::MICROS_PER_HOUR;
		const auto day = DateTrunc::FloorDiv(micros, Interval::MICROS_PER_DAY) - ZoneLUT::FIRST_DAY;
		if (day < 0 || day + 1 >= ZoneLUT::DAY_COUNT) {
			ThrowBucketRange();
		}
		const auto value = FixUpHour(lut.InstantEntry(day), hour);
		return value == hour ? FixUpHour(lut.InstantEntry(day + 1), hour) : value;
	}

	static bool HourNeedsTransitions(const ZoneLUT &lut, const BucketSpec &spec) {
		return spec.kind == BucketSpec::Kind::INSTANT && spec.part == DatePartSpecifier::HOUR && !lut.HasFixedOffset();
	}

	template <class FUNC>
	static void DispatchBucket(const ZoneLUT &lut, const BucketSpec &spec, FUNC &&func) {
		const auto width = spec.width;
		const auto anchor = spec.anchor;
		switch (spec.kind) {
		case BucketSpec::Kind::INSTANT:
			if (HourNeedsTransitions(lut, spec)) {
				func([&](int64_t micros) {
					return DateTrunc::FloorDiv(HourWithTransitions(lut, micros) - anchor, width);
				});
			} else {
				func([&](int64_t micros) { return DateTrunc::FloorDiv(micros - anchor, width); });
			}
			break;
		case BucketSpec::Kind::LOCAL_DAY:
			func([&](int64_t micros) { return DateTrunc::FloorDiv(LocalDay(lut, micros) - anchor, width); });
			break;
		default:
			func([&](int64_t micros) {
				return DateTrunc::FloorDiv(DateTrunc::MonthIndex(LocalDay(lut, micros)) - anchor, width);
			});
			break;
		}
	}

	static int64_t BucketOf(const ZoneLUT &lut, const BucketSpec &spec, int64_t micros) {
		int64_t result = 0;
		DispatchBucket(lut, spec, [&](auto bucket) { result = bucket(micros); });
		return result;
	}

	static timestamp_tz_t UnbucketOf(const BindData &info, const BucketSpec &spec, int64_t bucket,
	                                 CalendarPtr &calendar) {
		const auto &lut = *info.lut;
		const int64_t value = bucket * spec.width + spec.anchor;
		int64_t day = 0;
		switch (spec.kind) {
		case BucketSpec::Kind::INSTANT:
			return timestamp_tz_t(value);
		case BucketSpec::Kind::LOCAL_DAY:
			day = value;
			break;
		default:
			day = DateTrunc::MonthIndexStartDays(value);
			break;
		}
		int64_t representative = 0;
		if (!lut.TryResolveDay(day - ZoneLUT::FIRST_DAY, day * Interval::MICROS_PER_DAY + Interval::MICROS_PER_DAY / 2,
		                       representative)) {
			ThrowBucketRange();
		}
		const timestamp_tz_t input(representative);
		timestamp_tz_t result;
		if (DateTrunc::Dispatch(spec.part, [&](auto op) { return TryTruncate<decltype(op)>(lut, input, result); })) {
			return result;
		}
		if (!calendar) {
			calendar.reset(info.calendar->clone());
		}
		auto truncator = TruncationFactory(spec.part);
		auto micros = SetTime(calendar.get(), input);
		truncator(calendar.get(), micros);
		return GetTimeUnsafe(calendar.get(), micros);
	}

	static int64_t BucketFirstDay(const ZoneLUT &lut, const BucketSpec &spec) {
		switch (spec.kind) {
		case BucketSpec::Kind::INSTANT:
			if (spec.part == DatePartSpecifier::HOUR) {
				return lut.HourBucketFirstDay();
			}
			if (spec.part == DatePartSpecifier::MINUTE) {
				return lut.MinuteBucketFirstDay();
			}
			return lut.HasFixedOffset() && lut.FixedOffset() % Interval::MICROS_PER_SEC != 0 ? ZoneLUT::DAY_COUNT : 0;
		default:
			return lut.DayBucketFirstDay();
		}
	}

	static BucketSpec GetBucketSpec(Vector &part_arg) {
		if (part_arg.GetVectorType() != VectorType::CONSTANT_VECTOR || ConstantVector::IsNull(part_arg)) {
			throw InvalidInputException("Time zone bucket functions need a constant part");
		}
		BucketSpec spec;
		const auto part = GetDatePartSpecifier(ConstantVector::GetData<string_t>(part_arg)->GetString());
		if (!TryGetBucketSpec(part, spec)) {
			throw InvalidInputException("Time zone bucket functions do not support this part");
		}
		return spec;
	}

	static const BindData &GetBucketBindData(ExpressionState &state) {
		auto &info = state.expr.Cast<BoundFunctionExpression>().BindInfo()->Cast<BindData>();
		if (!info.lut || !info.lut->IsValid()) {
			throw InvalidInputException("Time zone bucket functions need a Gregorian calendar");
		}
		return info;
	}

	static void BucketFunction(DataChunk &args, ExpressionState &state, Vector &result) {
		const auto &info = GetBucketBindData(state);
		const auto spec = GetBucketSpec(args.data[0]);
		const auto &lut = *info.lut;
		DispatchBucket(lut, spec, [&](auto bucket) {
			UnaryExecutor::Execute<timestamp_tz_t, int64_t>(args.data[1], result, args.size(),
			                                                [&](timestamp_tz_t input) { return bucket(input.value); });
		});
	}

	static void UnbucketFunction(DataChunk &args, ExpressionState &state, Vector &result) {
		const auto &info = GetBucketBindData(state);
		const auto spec = GetBucketSpec(args.data[0]);
		CalendarPtr calendar;
		UnaryExecutor::Execute<int64_t, timestamp_tz_t>(args.data[1], result, args.size(), [&](int64_t bucket) {
			return UnbucketOf(info, spec, bucket, calendar);
		});
	}

	static ScalarFunction GetBucketFunction() {
		return ScalarFunction(Identifier("__internal_icu_date_trunc_bucket"),
		                      {LogicalType::VARCHAR, LogicalType::TIMESTAMP_TZ}, LogicalType::BIGINT, BucketFunction,
		                      Bind);
	}

	static ScalarFunction GetUnbucketFunction() {
		return ScalarFunction(Identifier("__internal_icu_date_trunc_unbucket"),
		                      {LogicalType::VARCHAR, LogicalType::BIGINT}, LogicalType::TIMESTAMP_TZ, UnbucketFunction,
		                      Bind);
	}

	class ICUBucketRewrite : public BucketRewrite {
	public:
		ICUBucketRewrite(BucketSpec spec_p, const BindData &info_p, Value part_p)
		    : spec(spec_p), info(make_uniq<BindData>(info_p)), part(std::move(part_p)) {
		}

		idx_t InputIndex() const override {
			return 1;
		}
		bool TryBucketRange(const BaseStatistics &stats, int64_t &min_bucket, int64_t &max_bucket) const override {
			if (!NumericStats::HasMinMax(stats)) {
				return false;
			}
			const auto min = NumericStats::GetMin<timestamp_tz_t>(stats);
			const auto max = NumericStats::GetMax<timestamp_tz_t>(stats);
			if (min > max || !min.IsFinite() || !max.IsFinite()) {
				return false;
			}
			const auto &lut = *info->lut;
			const auto first_day = MaxValue<int64_t>(BucketFirstDay(lut, spec), 1);
			const auto min_day = DateTrunc::FloorDiv(min.value, Interval::MICROS_PER_DAY) - ZoneLUT::FIRST_DAY;
			const auto max_day = DateTrunc::FloorDiv(max.value, Interval::MICROS_PER_DAY) - ZoneLUT::FIRST_DAY;
			if (min_day < first_day || max_day + 1 >= ZoneLUT::DAY_COUNT) {
				return false;
			}
			min_bucket = BucketOf(lut, spec, min.value);
			max_bucket = BucketOf(lut, spec, max.value);
			return true;
		}
		unique_ptr<Expression> Bucket(unique_ptr<Expression> input) const override {
			return MakeCall(GetBucketFunction(), std::move(input));
		}
		unique_ptr<Expression> Unbucket(unique_ptr<Expression> bucket) const override {
			return MakeCall(GetUnbucketFunction(), std::move(bucket));
		}

	private:
		unique_ptr<Expression> MakeCall(ScalarFunction function, unique_ptr<Expression> input) const {
			vector<unique_ptr<Expression>> arguments;
			arguments.push_back(make_uniq<BoundConstantExpression>(part));
			arguments.push_back(std::move(input));
			BoundScalarFunction bound_function(std::move(function));
			return make_uniq<BoundFunctionExpression>(std::move(bound_function), std::move(arguments),
			                                          make_uniq<BindData>(*info));
		}

		BucketSpec spec;
		unique_ptr<BindData> info;
		Value part;
	};

	static unique_ptr<BucketRewrite> BucketRewriteCallback(ClientContext &context, const BoundFunctionExpression &expr) {
		auto &children = expr.GetChildren();
		if (children.size() != 2 || children[0]->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT ||
		    children[1]->GetReturnType().id() != LogicalTypeId::TIMESTAMP_TZ || !expr.BindInfo()) {
			return nullptr;
		}
		auto &info = expr.BindInfo()->Cast<BindData>();
		if (!info.lut || !info.lut->IsValid()) {
			return nullptr;
		}
		const auto &part = children[0]->Cast<BoundConstantExpression>().GetValue();
		DatePartSpecifier specifier;
		BucketSpec spec;
		if (part.IsNull() || !TryGetDatePartSpecifier(StringValue::Get(part), specifier) ||
		    !TryGetBucketSpec(specifier, spec)) {
			return nullptr;
		}
		return make_uniq<ICUBucketRewrite>(spec, info, part);
	}

	template <typename TA>
	static ScalarFunction GetDateTruncFunction(const LogicalTypeId &type) {
		ScalarFunction function({LogicalType::VARCHAR, type}, LogicalType::TIMESTAMP_TZ, ICUDateTruncFunction<TA>, Bind);
		function.SetBucketRewriteCallback(BucketRewriteCallback);
		return function;
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
	loader.RegisterFunction(ICUDateTrunc::GetBucketFunction());
	loader.RegisterFunction(ICUDateTrunc::GetUnbucketFunction());
}

} // namespace duckdb
