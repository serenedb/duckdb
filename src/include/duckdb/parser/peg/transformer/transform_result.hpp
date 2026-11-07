#pragma once

#include "duckdb/common/common.hpp"

#include <cstring>

namespace duckdb {

template <class T>
struct TransformResultTypeIdentifier {
	static const char *GetName() {
		static_assert(AlwaysFalse<T>::VALUE,
		              "Transform result types must be registered with DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE");
		return nullptr;
	}
};

//! Registers a stable name for a transform result type. Invoke this macro from namespace duckdb.
#define DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE(NAME, ...)                                                               \
	template <>                                                                                                        \
	struct TransformResultTypeIdentifier<__VA_ARGS__> {                                                                \
		static constexpr const char *GetName() {                                                                       \
			return NAME;                                                                                               \
		}                                                                                                              \
	};

//! A stable per-type name, used to identify transform results across loadable extension boundaries without RTTI.
template <class T>
const char *TransformResultTypeName() {
	return TransformResultTypeIdentifier<T>::GetName();
}

constexpr uint64_t TransformResultTypeHash(const char *name) {
	uint64_t hash = 14695981039346656037ULL;
	for (; *name; name++) {
		hash = (hash ^ static_cast<uint8_t>(*name)) * 1099511628211ULL;
	}
	return hash;
}

struct TransformResultType {
	const char *name;
	uint64_t hash;
};

template <class T>
inline constexpr TransformResultType TRANSFORM_RESULT_TYPE = {
    TransformResultTypeIdentifier<T>::GetName(), TransformResultTypeHash(TransformResultTypeIdentifier<T>::GetName())};

struct DUCKDB_API TransformResultValue {
	TransformResultValue(const TransformResultType &type_p, void *value_p) : type(type_p), value_pointer(value_p) {
	}
	virtual ~TransformResultValue() = default;

	//! Returns a pointer to the value if its type matches, without relying on RTTI
	void *GetValuePointer(const TransformResultType &target) const {
		if (&target != &type && (target.hash != type.hash || std::strcmp(target.name, type.name) != 0)) {
			return nullptr;
		}
		return value_pointer;
	}
	void *GetValuePointer(const char *type_name) const {
		return GetValuePointer(TransformResultType {type_name, TransformResultTypeHash(type_name)});
	}

private:
	const TransformResultType &type;
	void *value_pointer;
};

template <class T>
struct DUCKDB_API TypedTransformResult : public TransformResultValue {
	explicit TypedTransformResult(T value_p)
	    : TransformResultValue(TRANSFORM_RESULT_TYPE<T>, &value), value(std::move(value_p)) {
	}
	TypedTransformResult(const TypedTransformResult &) = delete;
	TypedTransformResult &operator=(const TypedTransformResult &) = delete;

	T value;
};

//! Returns a pointer to the contained value if the result holds exactly T, and nullptr otherwise
template <class T>
T *TryGetTransformResult(TransformResultValue &result) {
	return reinterpret_cast<T *>(result.GetValuePointer(TRANSFORM_RESULT_TYPE<T>));
}

} // namespace duckdb
