//===----------------------------------------------------------------------===//
//                         DuckDB
//
// text_transform.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "text_casing.hpp"
#include "text_normalizer.hpp"

#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/storage/shared_object_cache.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace duckdb {
namespace text {

struct TransformOptions {
	NormalizationForm form = NormalizationForm::NFC;
	CaseMapping case_mapping = CaseMapping::NONE;
	CaseLocale locale = CaseLocale::ROOT;
	CaseFolding folding = CaseFolding::DEFAULT;
	bool strip_marks = false;
	bool strip_before_case = false;
	std::vector<uint32_t> keep_marks;
};

class TransformOutput {
public:
	TransformOutput(char *data_p, size_t capacity_p) : data(data_p), capacity(capacity_p) {
	}
	virtual ~TransformOutput() = default;

	virtual void Grow(size_t used, size_t needed) = 0;

	char *data;
	size_t capacity;
};

struct TransformBuffer {
	std::vector<uint32_t> text;
	std::vector<uint32_t> scratch;
};

class Transform {
public:
	explicit Transform(const TransformOptions &options, SharedObjectCache &cache);

	size_t Apply(std::string_view input, TransformOutput &output, TransformBuffer &buffer) const;

private:
	struct Tables {
		static constexpr uint16_t UNSTABLE = 0xFFFF;

		uint16_t ascii[0x80];
		uint16_t two_byte[0x800];
	};

	struct CachedTables final : ObjectCacheEntry {
		static constexpr std::string_view ObjectType() {
			return "text_transform_tables";
		}

		std::string GetObjectType() final {
			return std::string(ObjectType());
		}

		optional_idx GetEstimatedCacheMemory() const final {
			return sizeof(CachedTables);
		}

		Tables tables;
	};

	struct Classified {
		uint32_t code_point;
		uint32_t mapped;
	};

	enum class AsciiMode : uint8_t { IDENTITY, LOWER, UPPER, TABLE };

	shared_ptr<const Tables> GetTables(SharedObjectCache &cache) const;
	const Tables &GetSlotTables() const;
	void BuildTables(Tables &built, std::span<const uint32_t> keep) const;
	Classified Classify(const uint8_t *bytes, size_t size, size_t &position) const;
	bool IsWideStable(uint32_t c) const;
	uint32_t StableMapping(uint32_t c, TransformBuffer &buffer, std::span<const uint32_t> keep) const;
	void MapNormalized(TransformBuffer &buffer, uint32_t before, uint32_t after, std::span<const uint32_t> keep) const;
	void MapCase(TransformBuffer &buffer, uint32_t before, uint32_t after) const;
	void StripMarks(TransformBuffer &buffer, std::span<const uint32_t> keep) const;
	size_t ApplyWhole(std::string_view input, TransformOutput &output, TransformBuffer &buffer) const;

	TransformOptions options;
	NormalizationForm renormalize_form;
	NormalizationForm strip_form;
	bool composes;
	bool sigma_context;
	bool whole_value;
	AsciiMode ascii_mode;
	shared_ptr<const Tables> tables;
};

} // namespace text
} // namespace duckdb
