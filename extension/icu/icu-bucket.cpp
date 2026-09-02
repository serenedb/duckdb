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
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/function/scalar/date_bucket_rewrite.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"
#include "include/icu-datefunc.hpp"
#include "include/icu-datetrunc-lut.hpp"
#include "include/icu-timebucket-fast.hpp"
#include "include/icu-zone-lut.hpp"

namespace duckdb {

namespace {

struct ICUBucket : public ICUDateFunc {
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

	class Rewrite : public BucketRewrite {
	public:
		Rewrite(BucketSpec spec_p, const BindData &info_p, Value part_p)
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

	protected:
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

	static unique_ptr<BucketRewrite> DateTruncRewrite(ClientContext &context, const BoundFunctionExpression &expr) {
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
		return make_uniq<Rewrite>(spec, info, part);
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
		if (children.size() < 2 || children.size() > 3 ||
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
			default:
				return nullptr;
			}
		}
		return make_uniq<DateBucketRewrite>(context, spec, 1, LogicalType::TIMESTAMP_TZ, LogicalType::TIMESTAMP_TZ,
		                                    true);
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

void RegisterICUBucketFunctions(ExtensionLoader &loader) {
	loader.RegisterFunction(ICUBucket::GetBucketFunction());
	loader.RegisterFunction(ICUBucket::GetUnbucketFunction());
}

} // namespace duckdb
