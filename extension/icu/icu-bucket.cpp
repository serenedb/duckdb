////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2026 SereneDB GmbH, Berlin, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is SereneDB GmbH, Berlin, Germany
////////////////////////////////////////////////////////////////////////////////

#include "include/icu-bucket.hpp"

#include "duckdb/common/operator/date_trunc_operators.hpp"
#include "duckdb/common/optional.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/function/scalar/date_bucket_rewrite.hpp"
#include "duckdb/function/scalar/date_functions.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"
#include "include/icu-constant-args.hpp"
#include "include/icu-datefunc.hpp"
#include "include/icu-datepart-lut.hpp"
#include "include/icu-datetrunc-lut.hpp"
#include "include/icu-helpers.hpp"
#include "include/icu-origin-day.hpp"
#include "include/icu-timebucket-fast.hpp"
#include "include/icu-zone-lut.hpp"
#include "unicode/ucal.h"

#include <numeric>
#include <string_view>

namespace duckdb {

namespace {

struct ICUBucket : public ICUDateFunc {
	struct BucketSpec {
		enum class Kind : uint8_t { INSTANT, LOCAL_DAY, LOCAL_MONTH };
		Kind kind = Kind::INSTANT;
		DatePartSpecifier part = DatePartSpecifier::HOUR;
		int64_t width = 1;
		int64_t anchor = 0;
		bool origin_days = false;

		int64_t OriginDay() const {
			return kind == Kind::LOCAL_MONTH ? DateTrunc::MonthIndexStartDays(anchor) : anchor;
		}
	};

	static constexpr std::string_view TIME_BUCKET_DAY = "time_bucket_day";
	static constexpr std::string_view TIME_BUCKET_MONTH = "time_bucket_month";

	static bool TryParseBucketPart(std::string_view name, BucketSpec &spec) {
		if (name == TIME_BUCKET_DAY || name == TIME_BUCKET_MONTH) {
			const auto part = name == TIME_BUCKET_DAY ? DatePartSpecifier::DAY : DatePartSpecifier::MONTH;
			TryGetBucketSpec(part, spec);
			spec.origin_days = true;
			return true;
		}
		return TryGetBucketSpec(GetDatePartSpecifier(string(name)), spec);
	}

	static bool TryGetBucketSpec(DatePartSpecifier part, BucketSpec &spec) {
		spec = BucketSpec();
		spec.part = part;
		DateTruncUnit unit;
		if (!DateTrunc::TryGetUnit(part, unit)) {
			return false;
		}
		switch (unit.unit) {
		case DateTruncUnit::Unit::MICROS:
			spec.kind = BucketSpec::Kind::INSTANT;
			break;
		case DateTruncUnit::Unit::DAYS:
			spec.kind = BucketSpec::Kind::LOCAL_DAY;
			break;
		case DateTruncUnit::Unit::MONTHS:
			spec.kind = BucketSpec::Kind::LOCAL_MONTH;
			break;
		}
		spec.width = unit.width;
		spec.anchor = unit.anchor;
		return true;
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

	[[gnu::always_inline]] static inline int64_t OriginDay(const ZoneLUT &lut, int64_t micros) {
		int64_t day = 0;
		if (!ICUOriginDay::TryOriginDay(lut, micros, day)) {
			ThrowBucketRange();
		}
		return day;
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
			if (spec.origin_days) {
				func([&](int64_t micros) { return DateTrunc::FloorDiv(OriginDay(lut, micros) - anchor, width); });
			} else {
				func([&](int64_t micros) { return DateTrunc::FloorDiv(LocalDay(lut, micros) - anchor, width); });
			}
			break;
		default:
			if (spec.origin_days) {
				func([&](int64_t micros) {
					return DateTrunc::FloorDiv(DateTrunc::MonthIndex(OriginDay(lut, micros)) - anchor, width);
				});
			} else {
				func([&](int64_t micros) {
					return DateTrunc::FloorDiv(DateTrunc::MonthIndex(LocalDay(lut, micros)) - anchor, width);
				});
			}
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
		if (spec.origin_days) {
			return timestamp_tz_t(representative);
		}
		return ICUDateTruncLUT::Truncate(info, calendar, spec.part, timestamp_tz_t(representative));
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

	static int64_t ConstantArgument(Vector &arg, const char *what) {
		int64_t value = 0;
		if (!ICUConstantArgs::TryGet(arg, value)) {
			throw InvalidInputException("Time zone bucket functions need a constant %s", what);
		}
		return value;
	}

	static BucketSpec GetBucketSpec(DataChunk &args) {
		std::string_view part_name;
		if (!ICUConstantArgs::TryGetView(args.data[0], part_name)) {
			throw InvalidInputException("Time zone bucket functions need a constant part");
		}
		BucketSpec spec;
		if (!TryParseBucketPart(part_name, spec)) {
			throw InvalidInputException("Time zone bucket functions do not support this part");
		}
		if (args.ColumnCount() == 4) {
			spec.width = ConstantArgument(args.data[2], "width");
			spec.anchor = ConstantArgument(args.data[3], "anchor");
			if (spec.width <= 0) {
				throw InvalidInputException("Time zone bucket width must be positive");
			}
		}
		return spec;
	}

	static optional_ptr<BindData> ZonedBindData(const BoundFunctionExpression &expr, idx_t input_index,
	                                            idx_t min_children, idx_t max_children) {
		auto &children = expr.GetChildren();
		if (children.size() < min_children || children.size() > max_children ||
		    children[input_index]->GetReturnType().id() != LogicalTypeId::TIMESTAMP_TZ || !expr.BindInfo()) {
			return nullptr;
		}
		auto &info = expr.BindInfo()->Cast<BindData>();
		if (!info.lut || !info.lut->IsValid()) {
			return nullptr;
		}
		return &info;
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
		const auto spec = GetBucketSpec(args);
		const auto &lut = *info.lut;
		DispatchBucket(lut, spec, [&](auto bucket) {
			UnaryExecutor::Execute<timestamp_tz_t, int64_t>(args.data[1], result, args.size(),
			                                                [&](timestamp_tz_t input) { return bucket(input.value); });
		});
	}

	static void UnbucketFunction(DataChunk &args, ExpressionState &state, Vector &result) {
		const auto &info = GetBucketBindData(state);
		const auto spec = GetBucketSpec(args);
		CalendarPtr calendar;
		UnaryExecutor::Execute<int64_t, timestamp_tz_t>(args.data[1], result, args.size(), [&](int64_t bucket) {
			return UnbucketOf(info, spec, bucket, calendar);
		});
	}

	static ScalarFunctionSet BucketFunctions(const char *name, const LogicalType &input, const LogicalType &output,
	                                         scalar_function_t function) {
		ScalarFunctionSet set(name);
		set.AddFunction(ScalarFunction(Identifier(name), {LogicalType::VARCHAR, input}, output, function, Bind));
		set.AddFunction(ScalarFunction(Identifier(name),
		                               {LogicalType::VARCHAR, input, LogicalType::BIGINT, LogicalType::BIGINT}, output,
		                               function, Bind));
		return set;
	}

	static ScalarFunctionSet GetBucketFunctions() {
		return BucketFunctions("__internal_icu_date_trunc_bucket", LogicalType::TIMESTAMP_TZ, LogicalType::BIGINT,
		                       BucketFunction);
	}

	static ScalarFunctionSet GetUnbucketFunctions() {
		return BucketFunctions("__internal_icu_date_trunc_unbucket", LogicalType::BIGINT, LogicalType::TIMESTAMP_TZ,
		                       UnbucketFunction);
	}

	static ScalarFunction GetBucketFunction(bool bounded = false) {
		return GetBucketFunctions().functions[bounded ? 1 : 0];
	}

	static ScalarFunction GetUnbucketFunction(bool bounded = false) {
		return GetUnbucketFunctions().functions[bounded ? 1 : 0];
	}

	class Rewrite : public GranularBucketRewrite {
	public:
		Rewrite(BucketSpec spec_p, const BindData &info_p, Value part_p)
		    : spec(spec_p), info(make_uniq<BindData>(info_p)), part(std::move(part_p)) {
		}

		idx_t InputIndex() const override {
			return 1;
		}
		bool Contains(const GranularBucketRewrite &finer) const override {
			auto core = finer.Core();
			auto other = core ? dynamic_cast<const Rewrite *>(core.get()) : nullptr;
			if (!other || spec.origin_days != other->spec.origin_days || info->tz_setting != other->info->tz_setting) {
				return false;
			}
			const auto &fine = other->spec;
			switch (spec.kind) {
			case BucketSpec::Kind::INSTANT:
				return fine.kind == BucketSpec::Kind::INSTANT && info->lut && info->lut->HasFixedOffset() &&
				       Nested(spec.width, spec.anchor, fine.width, fine.anchor);
			case BucketSpec::Kind::LOCAL_DAY:
				if (fine.kind == BucketSpec::Kind::LOCAL_DAY) {
					return Nested(spec.width, spec.anchor, fine.width, fine.anchor);
				}
				return fine.kind == BucketSpec::Kind::INSTANT;
			default:
				if (fine.kind == BucketSpec::Kind::LOCAL_MONTH) {
					return Nested(spec.width, spec.anchor, fine.width, fine.anchor);
				}
				return fine.kind == BucketSpec::Kind::INSTANT || (fine.kind == BucketSpec::Kind::LOCAL_DAY && fine.width == 1);
			}
		}
		int64_t GranularityMicros() const override {
			if (spec.kind != BucketSpec::Kind::INSTANT) {
				return Interval::MICROS_PER_DAY;
			}
			return std::gcd(spec.width, spec.anchor);
		}
		unique_ptr<BucketRewrite> TryTimeOfDay(ClientContext &context, Expression &input) const override {
			Value session_zone;
			if (!context.TryGetCurrentSetting("TimeZone", session_zone) || session_zone.IsNull() ||
			    session_zone.ToString() != info->tz_setting || !info->lut) {
				return nullptr;
			}
			if (spec.kind != BucketSpec::Kind::INSTANT || BucketFirstDay(*info->lut, spec) != 0 ||
			    !TimeOfDayGrid(spec.width, spec.anchor)) {
				return nullptr;
			}
			return ZonedTimeOfDay(context, *info, spec.width, Value(), &input);
		}
		void RequireYearSpanBelow(int64_t years) {
			max_year_span = years;
		}
		bool TryBucketRange(const BaseStatistics &stats, int64_t &min_bucket, int64_t &max_bucket) const override {
			int64_t min_micros = 0;
			int64_t max_micros = 0;
			bool zoned = false;
			if (!TryGetMicrosRange(stats, min_micros, max_micros, zoned)) {
				return false;
			}
			if (!zoned) {
				min_micros -= Interval::MICROS_PER_DAY;
				max_micros += Interval::MICROS_PER_DAY;
			}
			const timestamp_tz_t min(min_micros);
			const timestamp_tz_t max(max_micros);
			if (max_year_span && DateTrunc::ToYearDay(DateTrunc::ToDays(timestamp_t(max.value))).year -
			                             DateTrunc::ToYearDay(DateTrunc::ToDays(timestamp_t(min.value))).year >=
			                         max_year_span - 1) {
				return false;
			}
			const auto &lut = *info->lut;
			const auto first_day = MaxValue<int64_t>(BucketFirstDay(lut, spec), 1);
			const auto min_day = DateTrunc::FloorDiv(min.value, Interval::MICROS_PER_DAY) - ZoneLUT::FIRST_DAY;
			const auto max_day = DateTrunc::FloorDiv(max.value, Interval::MICROS_PER_DAY) - ZoneLUT::FIRST_DAY;
			if (min_day < first_day || max_day + 1 >= ZoneLUT::DAY_COUNT) {
				return false;
			}
			if (spec.origin_days && !lut.OriginDaysSupported(spec.OriginDay(), min_day + ZoneLUT::FIRST_DAY - 1,
			                                                 max_day + ZoneLUT::FIRST_DAY + 1)) {
				return false;
			}
			min_bucket = BucketOf(lut, spec, min.value);
			max_bucket = BucketOf(lut, spec, max.value);
			return true;
		}
		unique_ptr<Expression> Bucket(unique_ptr<Expression> input) const override {
			return MakeCall(GetBucketFunction(HasExplicitBounds()), std::move(input));
		}
		unique_ptr<Expression> Unbucket(unique_ptr<Expression> bucket) const override {
			return MakeCall(GetUnbucketFunction(HasExplicitBounds()), std::move(bucket));
		}

	protected:
		bool HasExplicitBounds() const {
			BucketSpec base;
			return !TryGetBucketSpec(spec.part, base) || base.width != spec.width || base.anchor != spec.anchor;
		}

		unique_ptr<Expression> MakeCall(const ScalarFunction &function, unique_ptr<Expression> input) const {
			vector<unique_ptr<Expression>> arguments;
			arguments.push_back(make_uniq<BoundConstantExpression>(part));
			arguments.push_back(std::move(input));
			if (HasExplicitBounds()) {
				arguments.push_back(make_uniq<BoundConstantExpression>(Value::BIGINT(spec.width)));
				arguments.push_back(make_uniq<BoundConstantExpression>(Value::BIGINT(spec.anchor)));
			}
			return MakeBucketCall(function, std::move(arguments), make_uniq<BindData>(*info));
		}

		BucketSpec spec;
		unique_ptr<BindData> info;
		Value part;
		int64_t max_year_span = 0;
	};

	class DateCastRewrite : public Rewrite {
	public:
		DateCastRewrite(ClientContext &context_p, BucketSpec spec_p, const BindData &info_p)
		    : Rewrite(spec_p, info_p, Value("day")), context(context_p) {
		}

		idx_t InputIndex() const override {
			return 0;
		}
		unique_ptr<Expression> Unbucket(unique_ptr<Expression> bucket) const override {
			return BoundCastExpression::AddCastToType(context, Rewrite::Unbucket(std::move(bucket)), LogicalType::DATE);
		}

	private:
		ClientContext &context;
	};

	template <class LOCAL, class ZONED>
	static void ExecuteLocal(Vector &input, Vector &result, idx_t count, const BindData &info, LOCAL &&local_row,
	                         ZONED &&zoned_row) {
		const auto &lut = *info.lut;
		CalendarPtr calendar;
		UnaryExecutor::Execute<timestamp_tz_t, int64_t>(input, result, count,
		                                                [&](timestamp_tz_t input) -> optional<int64_t> {
			                                                if (!input.IsFinite()) {
				                                                return nullopt;
			                                                }
			                                                ICUDatePartLUT::LocalTime local;
			                                                if (ICUDatePartLUT::TryLocalTime(lut, input, local)) {
				                                                return local_row(local);
			                                                }
			                                                if (!calendar) {
				                                                calendar.reset(info.calendar->clone());
			                                                }
			                                                return zoned_row(calendar.get(), input);
		                                                });
	}

	static void CyclicFunction(DataChunk &args, ExpressionState &state, Vector &result) {
		const auto &info = GetBucketBindData(state);
		std::string_view part;
		if (!ICUConstantArgs::TryGetView(args.data[0], part)) {
			throw InvalidInputException("Time zone cyclic buckets need a constant part");
		}
		const bool month = part == "month";
		if (!month && part != "dow") {
			throw InvalidInputException("Time zone cyclic buckets support month and dow");
		}
		ExecuteLocal(
		    args.data[1], result, args.size(), info,
		    [&](const ICUDatePartLUT::LocalTime &local) {
			    return month ? ICUDatePartLUT::LocalMonth(local) : ICUDatePartLUT::LocalDayOfWeek(local);
		    },
		    [&](icu::Calendar *calendar, timestamp_tz_t input) {
			    SetTime(calendar, input);
			    return month ? int64_t(ExtractField(calendar, UCAL_MONTH)) + 1
			                 : int64_t(ExtractField(calendar, UCAL_DAY_OF_WEEK)) - 1;
		    });
	}

	static void TimeOfDayFunction(DataChunk &args, ExpressionState &state, Vector &result) {
		const auto &info = GetBucketBindData(state);
		const auto width = ConstantArgument(args.data[1], "width");
		if (width <= 0) {
			throw InvalidInputException("Time of day bucket width must be positive");
		}
		ExecuteLocal(
		    args.data[0], result, args.size(), info,
		    [&](const ICUDatePartLUT::LocalTime &local) { return local.time_of_day / width; },
		    [&](icu::Calendar *calendar, timestamp_tz_t input) {
			    const auto micros = SetTime(calendar, input);
			    const int64_t millis = ExtractField(calendar, UCAL_MILLISECONDS_IN_DAY);
			    return (millis * Interval::MICROS_PER_MSEC + int64_t(micros)) / width;
		    });
	}

	static ScalarFunction GetTimeOfDayFunction() {
		return ScalarFunction(Identifier("__internal_icu_time_of_day_bucket"),
		                      {LogicalType::TIMESTAMP_TZ, LogicalType::BIGINT}, LogicalType::BIGINT, TimeOfDayFunction,
		                      Bind);
	}

	static unique_ptr<BucketRewrite> ZonedTimeOfDay(ClientContext &context, const BindData &info, int64_t width,
	                                                const Value &format, optional_ptr<Expression> input) {
		return TimeOfDayRewrite(context, GetTimeOfDayFunction(), make_uniq<BindData>(info), 0, width, format, input);
	}

	static ScalarFunction GetCyclicFunction() {
		return ScalarFunction(Identifier("__internal_icu_cyclic_bucket"),
		                      {LogicalType::VARCHAR, LogicalType::TIMESTAMP_TZ}, LogicalType::BIGINT, CyclicFunction,
		                      Bind);
	}

	class CyclicRewrite : public CyclicBucketRewrite {
	public:
		CyclicRewrite(const BindData &info_p, string part_p, ScalarFunction unbucket, int64_t lo, int64_t hi)
		    : CyclicBucketRewrite(std::move(unbucket), lo, hi), info(make_uniq<BindData>(info_p)),
		      part(std::move(part_p)) {
		}

		unique_ptr<Expression> Bucket(unique_ptr<Expression> input) const override {
			vector<unique_ptr<Expression>> arguments;
			arguments.push_back(make_uniq<BoundConstantExpression>(Value(part)));
			arguments.push_back(std::move(input));
			return MakeBucketCall(GetCyclicFunction(), std::move(arguments), make_uniq<BindData>(*info));
		}

	private:
		unique_ptr<BindData> info;
		string part;
	};

	static unique_ptr<BucketRewrite> NameRewrite(const BoundFunctionExpression &expr, const string &part,
	                                             ScalarFunction unbucket, int64_t lo, int64_t hi) {
		auto info = ZonedBindData(expr, 0, 1, 1);
		if (!info) {
			return nullptr;
		}
		return make_uniq<CyclicRewrite>(*info, part, std::move(unbucket), lo, hi);
	}

	static std::string_view PartName(const BucketSpec &spec) {
		if (spec.origin_days) {
			return spec.kind == BucketSpec::Kind::LOCAL_MONTH ? TIME_BUCKET_MONTH : TIME_BUCKET_DAY;
		}
		return DateTruncPartName(spec.part);
	}

	static unique_ptr<BucketRewrite> StrfTimeRewrite(ClientContext &context, const BoundFunctionExpression &expr) {
		auto &children = expr.GetChildren();
		auto info = ZonedBindData(expr, 0, 2, 2);
		if (!info || children[1]->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
			return nullptr;
		}
		const auto &format_value = children[1]->Cast<BoundConstantExpression>().GetValue();
		if (format_value.IsNull() || format_value.type().id() != LogicalTypeId::VARCHAR) {
			return nullptr;
		}
		DatePartSpecifier part;
		BucketSpec spec;
		bool two_digit_year = false;
		if (!TryGetStrfTimeGranularity(StringValue::Get(format_value), false, part, two_digit_year) ||
		    !TryGetBucketSpec(part, spec)) {
			int64_t width = 0;
			if (!TryGetTimeOfDayWidth(StringValue::Get(format_value), width)) {
				return nullptr;
			}
			return ZonedTimeOfDay(context, *info, width, format_value, nullptr);
		}
		auto inner = make_uniq<Rewrite>(spec, *info, Value(DateTruncPartName(part)));
		if (two_digit_year) {
			inner->RequireYearSpanBelow(100);
		}
		return make_uniq<FunctionBucketRewrite>(std::move(inner), expr, 0);
	}

	static unique_ptr<BucketRewrite> LastDayRewrite(const BoundFunctionExpression &expr) {
		auto info = ZonedBindData(expr, 0, 1, 1);
		if (!info) {
			return nullptr;
		}
		BucketSpec spec;
		TryGetBucketSpec(DatePartSpecifier::MONTH, spec);
		auto inner = make_uniq<Rewrite>(spec, *info, Value("month"));
		return make_uniq<FunctionBucketRewrite>(std::move(inner), expr, 0);
	}

	static unique_ptr<BucketRewrite> DateTruncRewrite(ClientContext &context, const BoundFunctionExpression &expr) {
		auto &children = expr.GetChildren();
		auto info = ZonedBindData(expr, 1, 2, 2);
		if (!info || children[0]->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
			return nullptr;
		}
		const auto &part = children[0]->Cast<BoundConstantExpression>().GetValue();
		DatePartSpecifier specifier;
		BucketSpec spec;
		if (part.IsNull() || !TryGetDatePartSpecifier(StringValue::Get(part), specifier) ||
		    !TryGetBucketSpec(specifier, spec)) {
			return nullptr;
		}
		return make_uniq<Rewrite>(spec, *info, part);
	}

	static unique_ptr<BucketRewrite> DateCastRewriteCallback(ClientContext &context, const BoundCastExpression &cast) {
		if (cast.Child().GetReturnType().id() != LogicalTypeId::TIMESTAMP_TZ ||
		    cast.GetReturnType().id() != LogicalTypeId::DATE) {
			return nullptr;
		}
		auto cast_data = cast.GetBoundCast().GetCastData();
		if (!cast_data) {
			return nullptr;
		}
		auto &info = cast_data->Cast<CastData>().info->Cast<BindData>();
		if (!info.lut || !info.lut->IsValid()) {
			return nullptr;
		}
		BucketSpec spec;
		TryGetBucketSpec(DatePartSpecifier::DAY, spec);
		return make_uniq<DateCastRewrite>(context, spec, info);
	}

	static unique_ptr<BucketRewrite> TimeBucketRewrite(ClientContext &context, const BoundFunctionExpression &expr) {
		auto &children = expr.GetChildren();
		if (children.size() < 2 || children.size() > 4 ||
		    children[0]->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT ||
		    children[1]->GetReturnType().id() != LogicalTypeId::TIMESTAMP_TZ) {
			return nullptr;
		}
		const auto &width_value = children[0]->Cast<BoundConstantExpression>().GetValue();
		if (width_value.IsNull() || width_value.type().id() != LogicalTypeId::INTERVAL) {
			return nullptr;
		}
		const auto width = width_value.GetValue<interval_t>();
		DateBucketSpec spec;
		switch (ICUTimeBucketFast::Classify(width)) {
		case ICUTimeBucketFast::Kind::MICROS:
			spec.width = width.micros;
			spec.anchor = ICUTimeBucketFast::DEFAULT_ORIGIN_MICROS_1;
			break;
		case ICUTimeBucketFast::Kind::DAYS:
			spec.width = int64_t(width.days) * Interval::MICROS_PER_DAY;
			spec.anchor = ICUTimeBucketFast::DEFAULT_ORIGIN_MICROS_1;
			break;
		case ICUTimeBucketFast::Kind::MONTHS:
			spec.calendar = true;
			spec.width = width.months;
			spec.anchor = DateTrunc::MonthIndex(timestamp_t(ICUTimeBucketFast::DEFAULT_ORIGIN_MICROS_2));
			break;
		default:
			return nullptr;
		}
		if (children.size() == 3) {
			if (children[2]->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
				return nullptr;
			}
			const auto &third = children[2]->Cast<BoundConstantExpression>().GetValue();
			if (third.IsNull()) {
				return nullptr;
			}
			switch (third.type().id()) {
			case LogicalTypeId::INTERVAL: {
				const auto offset = third.GetValue<interval_t>();
				if (spec.calendar || offset.months != 0) {
					return nullptr;
				}
				spec.anchor += Interval::GetMicro(offset);
				break;
			}
			case LogicalTypeId::TIMESTAMP_TZ: {
				const auto origin = third.GetValue<timestamp_tz_t>();
				if (!origin.IsFinite() || !ICUTimeBucketFast::InRange(origin.value)) {
					return nullptr;
				}
				spec.anchor = spec.calendar ? DateTrunc::MonthIndex(timestamp_t(origin.value)) : origin.value;
				break;
			}
			case LogicalTypeId::VARCHAR:
				return ZonedTimeBucketRewrite(context, expr, width, StringValue::Get(third), timestamp_tz_t(0), false);
			default:
				return nullptr;
			}
		}
		if (children.size() == 4) {
			if (children[2]->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT ||
			    children[3]->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
				return nullptr;
			}
			const auto &zone = children[2]->Cast<BoundConstantExpression>().GetValue();
			const auto &origin_value = children[3]->Cast<BoundConstantExpression>().GetValue();
			if (zone.IsNull() || origin_value.IsNull() || zone.type().id() != LogicalTypeId::VARCHAR ||
			    origin_value.type().id() != LogicalTypeId::TIMESTAMP_TZ) {
				return nullptr;
			}
			const auto origin = origin_value.GetValue<timestamp_tz_t>();
			if (!origin.IsFinite() || !ICUTimeBucketFast::InRange(origin.value)) {
				return nullptr;
			}
			return ZonedTimeBucketRewrite(context, expr, width, StringValue::Get(zone), origin, true);
		}
		return make_uniq<DateBucketRewrite>(context, spec, 1, LogicalType::TIMESTAMP_TZ, LogicalType::TIMESTAMP_TZ,
		                                    true);
	}

	static unique_ptr<BucketRewrite> ZonedTimeBucketRewrite(ClientContext &context, const BoundFunctionExpression &expr,
	                                                        interval_t width, string tz, timestamp_tz_t origin,
	                                                        bool has_origin) {
		if (!ICUHelpers::TryGetTimeZone(tz) || !expr.BindInfo()) {
			return nullptr;
		}
		const BindData zoned(tz, expr.BindInfo()->Cast<BindData>().cal_setting);
		if (!zoned.lut || !zoned.lut->IsValid()) {
			return nullptr;
		}
		int64_t origin_day = 0;
		if (has_origin && !ICUOriginDay::TryOriginDay(*zoned.lut, origin.value, origin_day)) {
			return nullptr;
		}
		const bool origin_at_midnight = [&]() {
			int64_t start = 0;
			return ICUOriginDay::TryBucketStart(*zoned.lut, origin_day, start) && start == origin.value;
		}();
		BucketSpec spec;
		switch (ICUTimeBucketFast::Classify(width)) {
		case ICUTimeBucketFast::Kind::MICROS: {
			const auto anchor =
			    has_origin ? origin
			               : FromNaive(zoned.calendar.get(), timestamp_t(ICUTimeBucketFast::DEFAULT_ORIGIN_MICROS_1));
			DateBucketSpec fixed;
			fixed.width = width.micros;
			fixed.anchor = anchor.value;
			return make_uniq<DateBucketRewrite>(context, fixed, 1, LogicalType::TIMESTAMP_TZ,
			                                    LogicalType::TIMESTAMP_TZ, true);
		}
		case ICUTimeBucketFast::Kind::DAYS:
			if (has_origin && !origin_at_midnight) {
				return nullptr;
			}
			TryParseBucketPart(TIME_BUCKET_DAY, spec);
			spec.width = width.days;
			spec.anchor = has_origin ? origin_day : ICUTimeBucketFast::DEFAULT_ORIGIN_MICROS_1 / Interval::MICROS_PER_DAY;
			break;
		case ICUTimeBucketFast::Kind::MONTHS:
			if (has_origin &&
			    (!origin_at_midnight || origin_day != DateTrunc::MonthIndexStartDays(DateTrunc::MonthIndex(origin_day)))) {
				return nullptr;
			}
			TryParseBucketPart(TIME_BUCKET_MONTH, spec);
			spec.width = width.months;
			spec.anchor = has_origin ? DateTrunc::MonthIndex(origin_day)
			                         : DateTrunc::MonthIndex(timestamp_t(ICUTimeBucketFast::DEFAULT_ORIGIN_MICROS_2));
			break;
		default:
			return nullptr;
		}
		auto inner = make_uniq<Rewrite>(spec, zoned, Value(string(PartName(spec))));
		return make_uniq<FunctionBucketRewrite>(std::move(inner), expr, 1);
	}
};

} // namespace

unique_ptr<BucketRewrite> ICUDateTruncBucketRewrite(ClientContext &context, const BoundFunctionExpression &expr) {
	return ICUBucket::DateTruncRewrite(context, expr);
}

unique_ptr<BucketRewrite> ICUTimeBucketBucketRewrite(ClientContext &context, const BoundFunctionExpression &expr) {
	return ICUBucket::TimeBucketRewrite(context, expr);
}

unique_ptr<BucketRewrite> ICUDateCastBucketRewrite(ClientContext &context, const BoundCastExpression &cast) {
	return ICUBucket::DateCastRewriteCallback(context, cast);
}

unique_ptr<BucketRewrite> ICUStrfTimeBucketRewrite(ClientContext &context, const BoundFunctionExpression &expr) {
	return ICUBucket::StrfTimeRewrite(context, expr);
}

unique_ptr<BucketRewrite> ICUMonthNameBucketRewrite(ClientContext &context, const BoundFunctionExpression &expr) {
	return ICUBucket::NameRewrite(expr, "month", InternalMonthNameFun::GetFunction(), 1, 12);
}

unique_ptr<BucketRewrite> ICUDayNameBucketRewrite(ClientContext &context, const BoundFunctionExpression &expr) {
	return ICUBucket::NameRewrite(expr, "dow", InternalDayNameFun::GetFunction(), 0, 6);
}

unique_ptr<BucketRewrite> ICULastDayBucketRewrite(ClientContext &context, const BoundFunctionExpression &expr) {
	return ICUBucket::LastDayRewrite(expr);
}

void RegisterICUBucketFunctions(ExtensionLoader &loader) {
	loader.RegisterFunction(ICUBucket::GetBucketFunctions());
	loader.RegisterFunction(ICUBucket::GetUnbucketFunctions());
	loader.RegisterFunction(ICUBucket::GetCyclicFunction());
	loader.RegisterFunction(ICUBucket::GetTimeOfDayFunction());
}

} // namespace duckdb
